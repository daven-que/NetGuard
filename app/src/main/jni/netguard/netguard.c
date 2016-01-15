#include <jni.h>
#include <android/log.h>

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/udp.h>
#include <netinet/tcp.h>

// 3 way handshake
// -> SYN seq=x
// <- SYN-ACK ack=x+1 seq=y
// -> ACK=y+1 seq=x+1

// https://www.gasmi.net/hpd/
// Ethernet frame: 0800 2086 354b 00e0 f726 3fe9 0800

// TODO TCP fragmentation
// TODO TCP push
// TODO header file

#define TAG "NetGuard.JNI"
#define MAXPKT 32678
#define TIMEOUTPKT 30
#define TTL 64

struct arguments {
    jobject instance;
    int tun;
};

struct data {
    uint32_t len;
    uint8_t *data;
    struct data *next;
};

struct connection {
    time_t time;
    uint32_t remote_seq; // confirmed bytes received, host notation
    uint32_t local_seq; // confirmed bytes sent, host notation
    int32_t saddr; // network notation
    __be16 source; // network notation
    int32_t daddr; // network notation
    __be16 dest; // network notation
    uint8_t state;
    jint socket;
    uint32_t lport; // host notation
    struct data *sent;
    struct connection *next;
};

void *handle_events(void *);

void handle_tcp(JNIEnv *, jobject, const struct arguments *args, const uint8_t *, const uint16_t);

int openSocket(JNIEnv *, jobject, const struct sockaddr_in *);

int getLocalPort(const int);

int canWrite(const int);

int writeSYN(const struct connection *, const int);

int writeACK(const struct connection *, struct data *, uint16_t, int, int);

void decode(JNIEnv *, jobject, const struct arguments *args, const uint8_t *, const uint16_t);

jint getUid(const int, const int, const void *, const uint16_t);

unsigned short checksum(unsigned short *, int);

void nsleep(const long);

char *hex(const u_int8_t *, const u_int16_t);

// Global variables

static JavaVM *jvm;
pthread_t thread_id;
int signaled = 0;
struct connection *connection = NULL;

// JNI

JNIEXPORT void JNICALL
Java_eu_faircode_netguard_SinkholeService_jni_1init(JNIEnv *env, jobject instance) {
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Init");
    connection = NULL;
}

JNIEXPORT void JNICALL
Java_eu_faircode_netguard_SinkholeService_jni_1start(JNIEnv *env, jobject instance, jint tun) {
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Starting tun=%d", tun);

    if (pthread_kill(thread_id, 0) == 0)
        __android_log_print(ANDROID_LOG_WARN, TAG, "Already running thread %u", thread_id);
    else {
        jint rs = (*env)->GetJavaVM(env, &jvm);
        if (rs != JNI_OK)
            __android_log_print(ANDROID_LOG_ERROR, TAG, "GetJavaVM failed");

        struct arguments *args = malloc(sizeof(struct arguments));
        args->instance = (*env)->NewGlobalRef(env, instance);
        args->tun = tun;
        int err = pthread_create(&thread_id, NULL, handle_events, args);
        if (err != 0)
            __android_log_print(ANDROID_LOG_ERROR, TAG, "pthread_create error %d: %s",
                                err, strerror(err));
    }
}

JNIEXPORT void JNICALL
Java_eu_faircode_netguard_SinkholeService_jni_1stop(JNIEnv *env, jobject instance, jint tun) {
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Stop thread %u", thread_id);
    if (pthread_kill(thread_id, 0) == 0) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Kill thread %u", thread_id);
        int err = pthread_kill(thread_id, SIGUSR1);
        if (err != 0)
            __android_log_print(ANDROID_LOG_WARN, TAG, "pthread_kill error %d: %s",
                                err, strerror(err));
        else {
            __android_log_print(ANDROID_LOG_DEBUG, TAG, "Join thread %u", thread_id);
            pthread_join(thread_id, NULL);
            if (err != 0)
                __android_log_print(ANDROID_LOG_WARN, TAG, "pthread_join error %d: %s",
                                    err, strerror(err));
        }
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Stopped");
    } else
        __android_log_print(ANDROID_LOG_WARN, TAG, "Not running");
}

JNIEXPORT void JNICALL
Java_eu_faircode_netguard_SinkholeService_jni_1reload(JNIEnv *env, jobject instance, jint tun) {
    // TODO seamless handover
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Reload tun=%d", tun);
    Java_eu_faircode_netguard_SinkholeService_jni_1stop(env, instance, tun);
    Java_eu_faircode_netguard_SinkholeService_jni_1start(env, instance, tun);
}

// Private functions

void sig_handler(int sig, siginfo_t *info, void *context) {
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Signal %d", sig);
    signaled = 1;
}

void *handle_events(void *a) {
    struct arguments *args = (struct arguments *) a;
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Start events tun=%d thread %u", args->tun,
                        thread_id);

    JNIEnv *env;
    jint rs = (*jvm)->AttachCurrentThread(jvm, &env, NULL);
    if (rs != JNI_OK)
        __android_log_print(ANDROID_LOG_ERROR, TAG, "AttachCurrentThread failed");

    int max;
    fd_set rfds;
    fd_set wfds;
    fd_set efds;
    struct timespec ts;
    char dest[20];
    sigset_t blockset;
    sigset_t emptyset;
    struct sigaction sa;

    // Block SIGUSR1
    sigemptyset(&blockset);
    sigaddset(&blockset, SIGUSR1);
    sigprocmask(SIG_BLOCK, &blockset, NULL);

    /// Handle SIGUSR1
    sa.sa_sigaction = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);

    signaled = 0;

    // Loop
    while (1) {
        time_t now = time(NULL);
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Select thread %u", thread_id);

        // Select
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_ZERO(&efds);

        FD_SET(args->tun, &rfds);
        FD_SET(args->tun, &efds);

        max = args->tun;

        struct connection *last = NULL;
        struct connection *cur = connection;
        while (cur != NULL) {
            if (cur->time + TIMEOUTPKT < now) {
                // Log
                inet_ntop(AF_INET, &(cur->daddr), dest, sizeof(dest));
                __android_log_print(ANDROID_LOG_DEBUG, TAG, "Idle %s/%u lport %u",
                                    dest, ntohs(cur->dest), cur->lport);


                // TODO check if open
                shutdown(cur->socket, SHUT_RDWR);
                // TODO check for errors

                struct data *prev;
                struct data *sent = cur->sent;
                while (sent != NULL) {
                    prev = sent;
                    sent = sent->next;
                    if (prev->data != NULL)
                        free(prev->data);
                    free(prev);
                }

                if (last == NULL)
                    connection = cur->next;
                else
                    last->next = cur->next;

                struct connection *c = cur;
                cur = cur->next;
                free(c);
                continue;

            } else {
                if (cur->state == TCP_SYN_RECV) {
                    FD_SET(cur->socket, &wfds);
                    if (cur->socket > max)
                        max = cur->socket;
                }
                else if (cur->state == TCP_ESTABLISHED) {
                    FD_SET(cur->socket, &rfds);
                    if (cur->socket > max)
                        max = cur->socket;
                }
            }

            last = cur;
            cur = cur->next;
        }

        ts.tv_sec = 10;
        ts.tv_nsec = 0;
        sigemptyset(&emptyset);
        int ready = pselect(max + 1, &rfds, &wfds, &efds, &ts, &emptyset);
        if (ready < 0) {
            if (errno == EINTR) {
                if (signaled) { ;
                    __android_log_print(ANDROID_LOG_DEBUG, TAG, "pselect signaled");
                    break;
                } else {
                    __android_log_print(ANDROID_LOG_WARN, TAG, "pselect interrupted");
                    continue;
                }
            } else {
                __android_log_print(ANDROID_LOG_ERROR, TAG, "select error %d: %s",
                                    errno, strerror(errno));
                break;
            }
        }

        if (ready == 0)
            __android_log_print(ANDROID_LOG_DEBUG, TAG, "Yield");
        else {
            // Check tun exception
            if (FD_ISSET(args->tun, &efds)) {
                __android_log_print(ANDROID_LOG_ERROR, TAG, "tun exception");
                break;
            }

            // Check tun read
            if (FD_ISSET(args->tun, &rfds)) {
                uint8_t buffer[MAXPKT];
                ssize_t length = read(args->tun, buffer, MAXPKT);
                if (length < 0) {
                    __android_log_print(ANDROID_LOG_ERROR, TAG, "tun read error %d: %s",
                                        errno, strerror(errno));
                    break;
                }
                if (length > 0)
                    decode(env, args->instance, args, buffer, length);
                else {
                    __android_log_print(ANDROID_LOG_ERROR, TAG, "tun empty read");
                    break;
                }
            }

            // Check sockets
            struct connection *cur = connection;
            while (cur != NULL) {
                // Check socket exception
                if (FD_ISSET(cur->socket, &efds)) {
                    int serr;
                    socklen_t optlen = sizeof(serr);
                    if (getsockopt(cur->socket, SOL_SOCKET, SO_ERROR, &serr, &optlen) < 0) {
                        __android_log_print(ANDROID_LOG_ERROR, TAG, "getsockopt error %d: %s",
                                            errno, strerror(errno));
                        cur->state = TCP_CLOSE;
                        cur = cur->next;
                        continue;
                    }
                    if (serr) {
                        __android_log_print(ANDROID_LOG_ERROR, TAG, "SO_ERROR %d: %s",
                                            serr, strerror(serr));
                        cur->state = TCP_CLOSE;
                        cur = cur->next;
                        continue;
                    }
                }

                if (cur->state == TCP_SYN_RECV) {
                    // Check socket connect
                    if (FD_ISSET(cur->socket, &wfds) && canWrite(args->tun)) {
                        // Log
                        char dest[20];
                        inet_ntop(AF_INET, &(cur->daddr), dest, sizeof(dest));
                        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Established %s/%u lport %u",
                                            dest, ntohs(cur->dest), cur->lport);

                        if (writeSYN(cur, args->tun) < 0)
                            cur->state = TCP_CLOSE;
                        else
                            cur->state = TCP_SYN_SENT;
                    }
                }

                else if (cur->state == TCP_ESTABLISHED) {
                    // Check socket read
                    if (FD_ISSET(cur->socket, &rfds)) {
                        uint8_t buffer[MAXPKT];
                        ssize_t bytes = recv(cur->socket, buffer, MAXPKT, 0);
                        if (bytes < 0) {
                            __android_log_print(ANDROID_LOG_ERROR, TAG, "recv socket error %d: %s",
                                                errno, strerror(errno));
                            if (errno != EINTR)
                                cur->state = TCP_CLOSE;
                        }
                        else if (bytes == 0) {
                            __android_log_print(ANDROID_LOG_ERROR, TAG, "recv socket empty");
                            cur->state = TCP_CLOSE;
                        } else {
                            __android_log_print(ANDROID_LOG_DEBUG, TAG,
                                                "recv socket lport %u bytes %d",
                                                cur->lport, bytes);
                            struct data *data = malloc(sizeof(struct data));
                            data->len = bytes;
                            data->data = malloc(bytes);
                            memcpy(data->data, buffer, bytes);
                            // canWrite(args->tun)
                            writeACK(cur, data, 0, 0, args->tun);
                            // TODO check result
                            // TODO retransmits
                            free(data->data);
                            free(data);
                            cur->local_seq += bytes;
                        }
                    }
                }

                cur = cur->next;
            }
        }
    }

    (*env)->DeleteGlobalRef(env, args->instance);
    rs = (*jvm)->DetachCurrentThread(jvm);
    if (rs != JNI_OK)
        __android_log_print(ANDROID_LOG_ERROR, TAG, "DetachCurrentThread failed");
    free(args);

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Stopped events tun=%d thread %u",
                        args->tun, thread_id);
}

void handle_tcp(JNIEnv *env, jobject instance, const struct arguments *args,
                const uint8_t *buffer, uint16_t length) {
    // Check version
    uint8_t version = (*buffer) >> 4;
    if (version != 4)
        return;

    // Get headers
    struct iphdr *iphdr = buffer;
    uint8_t optlen = (iphdr->ihl - 5) * 4;
    struct tcphdr *tcphdr = buffer + sizeof(struct iphdr) + optlen;
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "optlen %d", optlen);

    if (ntohs(iphdr->tot_len) != length)
        __android_log_print(ANDROID_LOG_WARN, TAG, "Invalid length %u/%d", iphdr->tot_len, length);

    // Get data
    uint16_t dataoff = sizeof(struct iphdr) + optlen + sizeof(struct tcphdr);
    uint16_t datalen = length - dataoff;
    struct data *data = NULL;
    if (datalen > 0) {
        data = malloc(sizeof(struct data));
        data->len = datalen;
        data->data = malloc(datalen); // TODO free
        memcpy(data->data, buffer + dataoff, datalen);
        data->next = NULL;
    }

    // Search connection
    struct connection *last = NULL;
    struct connection *cur = connection;
    while (cur != NULL && !(cur->saddr == iphdr->saddr && cur->source == tcphdr->source)) {
        last = cur;
        cur = cur->next;
    }

    // Log
    char dest[20];
    inet_ntop(AF_INET, &(iphdr->daddr), dest, sizeof(dest));
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "%s/%u seq %u ack %u data %d",
                        dest, ntohs(tcphdr->dest),
                        ntohl(tcphdr->seq), ntohl(tcphdr->ack_seq), datalen);

    if (cur == NULL) {
        if (tcphdr->syn) {
            __android_log_print(ANDROID_LOG_DEBUG, TAG, "New SYN");

            // Register connection
            struct connection *syn = malloc(sizeof(struct connection)); // TODO check/free
            syn->time = time(NULL);
            syn->remote_seq = ntohl(tcphdr->seq); // ISN remote
            syn->local_seq = 123; // ISN local TODO randomize
            syn->saddr = iphdr->saddr;
            syn->source = tcphdr->source;
            syn->daddr = iphdr->daddr;
            syn->dest = tcphdr->dest;
            syn->state = TCP_SYN_RECV;
            syn->sent = NULL;
            syn->next = NULL;

            // Ignore data
            if (data != NULL) {
                free(data->data);
                free(data);
            }

            // Build target address
            struct sockaddr_in daddr;
            memset(&daddr, 0, sizeof(struct sockaddr_in));
            daddr.sin_family = AF_INET;
            daddr.sin_port = tcphdr->dest;
            daddr.sin_addr.s_addr = iphdr->daddr;

            // Open socket
            syn->socket = openSocket(env, instance, &daddr);
            if (syn->socket < 0)
                syn->state = TCP_CLOSE;
            else {
                syn->lport = getLocalPort(syn->socket);

                __android_log_print(ANDROID_LOG_DEBUG, TAG, "Connecting to %s/%u lport %u",
                                    dest, ntohs(tcphdr->dest), syn->lport);
            }

            if (last == NULL)
                connection = syn;
            else
                last->next = syn;
        }
        else
            __android_log_print(ANDROID_LOG_WARN, TAG, "Unknown connection");
    }
    else {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Existing connection lport %u", cur->lport);

        if (tcphdr->syn)
            __android_log_print(ANDROID_LOG_DEBUG, TAG, "Ignoring repeated SYN");

        if (tcphdr->ack) {
            cur->time = time(NULL);

            if (cur->state == TCP_SYN_SENT) {
                // TODO proper warp around
                if (ntohl(tcphdr->ack_seq) == cur->local_seq + 1 &&
                    ntohl(tcphdr->seq) >= cur->remote_seq + 1) {
                    cur->local_seq += 1;
                    cur->remote_seq += 1;
                    // TODO process data

                    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Established");
                    cur->state = TCP_ESTABLISHED;
                }
                else
                    __android_log_print(ANDROID_LOG_WARN, TAG, "Invalid seq/ack");
            }

            else if (cur->state == TCP_ESTABLISHED) {
                // TODO proper wrap around
                if (ntohl(tcphdr->seq) + 1 == cur->remote_seq)
                    // TODO respond to keepalive?
                    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Keep alive");
                else if (ntohl(tcphdr->seq) < cur->remote_seq)
                    __android_log_print(ANDROID_LOG_WARN, TAG, "Processed ack");
                else {
                    __android_log_print(ANDROID_LOG_DEBUG, TAG, "New ack");
                    if (data != NULL) {
                        // TODO non blocking
                        __android_log_print(ANDROID_LOG_DEBUG, TAG, "send socket data %u",
                                            data->len);
                        if (send(cur->socket, data->data, data->len, 0) < 0)
                            __android_log_print(ANDROID_LOG_ERROR, TAG, "send error %d: %s",
                                                errno, strerror(errno));
                        else {
                            if (writeACK(cur, NULL, data->len, 0, args->tun))
                                cur->remote_seq += data->len;
                        }
                    }
                }
            }

            else if (cur->state == TCP_LAST_ACK) {
                // TODO check seq/ack
                __android_log_print(ANDROID_LOG_DEBUG, TAG, "Full close");
                cur->state = TCP_CLOSE;
            }

            else {
                __android_log_print(ANDROID_LOG_WARN, TAG, "Ignored state %d", cur->state);
            }
        }

        if (tcphdr->fin) {
            if (cur->state == TCP_ESTABLISHED) {
                // TODO close socket
                __android_log_print(ANDROID_LOG_DEBUG, TAG, "Partial close");
                if (writeACK(cur, NULL, 1, 1, args->tun) >= 0) {
                    cur->local_seq += 1;
                    cur->remote_seq += 1;
                    cur->state = TCP_LAST_ACK;
                }
            }
        }

        if (tcphdr->rst) {
            cur->state = TCP_CLOSE;
        }
    }
}

int openSocket(JNIEnv *env, jobject instance, const struct sockaddr_in *daddr) {
    int sock = -1;

    // Get TCP socket
    // TODO socket options (SO_REUSEADDR, etc)
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "socket error %d: %s",
                            errno, strerror(errno));
        return -1;
    }

    // Protect
    jclass cls = (*env)->GetObjectClass(env, instance);
    jmethodID mid = (*env)->GetMethodID(env, cls, "protect", "(I)Z");
    if (mid == 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "protect not found");
        return -1;
    }
    else {
        jboolean isProtected = (*env)->CallBooleanMethod(env, instance, mid, sock);
        if (!isProtected)
            __android_log_print(ANDROID_LOG_ERROR, TAG, "protect failed");

        jthrowable ex = (*env)->ExceptionOccurred(env);
        if (ex) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, ex);
        }
    }

    // Set non blocking
    uint8_t flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "fcntl O_NONBLOCK error %d: %s",
                            errno, strerror(errno));
        return -1;
    }

    // Initiate connect
    int err = connect(sock, daddr, sizeof(struct sockaddr_in));
    if (err < 0 && errno != EINPROGRESS) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "connect error %d: %s",
                            errno, strerror(errno));
        return -1;
    }

    // Set blocking
    if (fcntl(sock, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "fcntl error %d: %s",
                            errno, strerror(errno));
        return -1;
    }

    return sock;
}

int getLocalPort(const int sock) {
    struct sockaddr_in sin;
    int len = sizeof(sin);
    if (getsockname(sock, &sin, &len) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "getsockname error %d: %s",
                            errno, strerror(errno));
        return -1;
    } else
        return ntohs(sin.sin_port);
}

int canWrite(const int fd) {
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    return (select(fd + 1, NULL, &wfds, NULL, &tv) > 0);
}

int writeSYN(const struct connection *cur, int tun) {
    // Build packet
    uint16_t len = sizeof(struct iphdr) + sizeof(struct tcphdr); // no data
    u_int8_t *buffer = calloc(len, 1);
    struct iphdr *ip = buffer;
    struct tcphdr *tcp = buffer + sizeof(struct iphdr);

    // Build IP header
    ip->version = 4;
    ip->ihl = sizeof(struct iphdr) >> 2;
    ip->tot_len = htons(len);
    ip->ttl = TTL;
    ip->protocol = IPPROTO_TCP;
    ip->saddr = cur->daddr;
    ip->daddr = cur->saddr;

    // Calculate IP checksum
    ip->check = checksum(ip, sizeof(struct iphdr));

    // Build TCP header
    tcp->source = cur->dest;
    tcp->dest = cur->source;
    tcp->seq = htonl(cur->local_seq);
    tcp->ack_seq = htonl(cur->remote_seq + 1); // TODO proper wrap around
    tcp->doff = sizeof(struct tcphdr) >> 2;
    tcp->syn = 1;
    tcp->ack = 1;
    tcp->window = htons(2048);

    // Calculate TCP checksum
    uint16_t clen = sizeof(struct ippseudo) + sizeof(struct tcphdr);
    uint8_t csum[clen];

    // Build pseudo header
    struct ippseudo *pseudo = csum;
    pseudo->ippseudo_src.s_addr = ip->saddr;
    pseudo->ippseudo_dst.s_addr = ip->daddr;
    pseudo->ippseudo_pad = 0;
    pseudo->ippseudo_p = ip->protocol;
    pseudo->ippseudo_len = htons(sizeof(struct tcphdr)); // no data

    // Copy TCP header
    memcpy(csum + sizeof(struct ippseudo), tcp, sizeof(struct tcphdr));

    tcp->check = checksum(csum, clen);

    char to[20];
    inet_ntop(AF_INET, &(ip->daddr), to, sizeof(to));

    // Send packet
    __android_log_print(ANDROID_LOG_DEBUG, TAG,
                        "Sending SYN+ACK to tun %s/%u seq %u ack %u",
                        to, ntohs(tcp->dest),
                        ntohl(tcp->seq), ntohl(tcp->ack_seq));
    int res = write(tun, buffer, len);
    if (res < 0) {
        // TODO handle EINTR
        __android_log_print(ANDROID_LOG_ERROR, TAG, "write error %d: %s",
                            errno, strerror(errno));
    }

    free(buffer);

    return res;
}

int writeACK(const struct connection *cur, struct data *data, uint16_t confirm, int fin, int tun) {
    // Build packet
    uint16_t datalen = (data == NULL ? 0 : data->len);
    uint16_t len = sizeof(struct iphdr) + sizeof(struct tcphdr) + datalen; // no data
    u_int8_t *buffer = calloc(len, 1);
    struct iphdr *ip = buffer;
    struct tcphdr *tcp = buffer + sizeof(struct iphdr);
    if (datalen)
        memcpy(buffer + sizeof(struct iphdr) + sizeof(struct tcphdr), data->data, data->len);

    // Build IP header
    ip->version = 4;
    ip->ihl = sizeof(struct iphdr) >> 2;
    ip->tot_len = htons(len);
    ip->ttl = TTL;
    ip->protocol = IPPROTO_TCP;
    ip->saddr = cur->daddr;
    ip->daddr = cur->saddr;

    // Calculate IP checksum
    ip->check = checksum(ip, sizeof(struct iphdr));

    // Build TCP header
    tcp->source = cur->dest;
    tcp->dest = cur->source;
    tcp->seq = htonl(cur->local_seq);
    tcp->ack_seq = htonl(cur->remote_seq + confirm); // TODO proper wrap around
    tcp->doff = sizeof(struct tcphdr) >> 2;
    tcp->ack = 1;
    tcp->fin = fin;
    tcp->window = htons(2048);

    // Calculate TCP checksum
    uint16_t clen = sizeof(struct ippseudo) + sizeof(struct tcphdr) + datalen;
    uint8_t csum[clen];

    // Build pseudo header
    struct ippseudo *pseudo = csum;
    pseudo->ippseudo_src.s_addr = ip->saddr;
    pseudo->ippseudo_dst.s_addr = ip->daddr;
    pseudo->ippseudo_pad = 0;
    pseudo->ippseudo_p = ip->protocol;
    pseudo->ippseudo_len = htons(sizeof(struct tcphdr) + datalen); // no data

    // Copy TCP header + data
    memcpy(csum + sizeof(struct ippseudo), tcp, sizeof(struct tcphdr));
    if (datalen)
        memcpy(csum + sizeof(struct ippseudo) + sizeof(struct tcphdr), data->data, data->len);

    tcp->check = checksum(csum, clen);

    char to[20];
    inet_ntop(AF_INET, &(ip->daddr), to, sizeof(to));

    // Send packet
    __android_log_print(ANDROID_LOG_DEBUG, TAG,
                        "Sending ACK%s to tun %s/%u seq %u ack %u data %u confirm %u",
                        (fin ? "/FIN" : ""), to, ntohs(tcp->dest),
                        ntohl(tcp->seq), ntohl(tcp->ack_seq), datalen, confirm);
    int res = write(tun, buffer, len);
    if (res < 0) {
        // TODO handle EINTR
        __android_log_print(ANDROID_LOG_ERROR, TAG, "write error %d: %s",
                            errno, strerror(errno));
    }

    free(buffer);

    return res;
}

void decode(JNIEnv *env, jobject instance, const struct arguments *args,
            const uint8_t *buffer, const uint16_t length) {
    uint8_t protocol;
    void *saddr;
    void *daddr;
    char source[40];
    char dest[40];
    char flags[10];
    int flen = 0;
    uint8_t *payload;

    // Get protocol, addresses & payload
    uint8_t version = (*buffer) >> 4;
    if (version == 4) {
        struct iphdr *ip4hdr = buffer;

        protocol = ip4hdr->protocol;
        saddr = &ip4hdr->saddr;
        daddr = &ip4hdr->daddr;

        if (ip4hdr->frag_off & IP_MF)
            flags[flen++] = '+';

        uint8_t optlen = (ip4hdr->ihl - 5) * 4;
        payload = buffer + 20 + optlen;

        if (ntohs(ip4hdr->tot_len) != length) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "Invalid length %u header length %u",
                                length, ntohs(ip4hdr->tot_len));
            return;
        }

        uint16_t csum = checksum(ip4hdr, sizeof(struct iphdr));
        if (csum != 0) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "Invalid IP checksum");
            return;
        }
    }
    else if (version == 6) {
        struct ip6_hdr *ip6hdr = buffer;

        protocol = ip6hdr->ip6_nxt;
        saddr = &ip6hdr->ip6_src;
        daddr = &ip6hdr->ip6_dst;

        payload = buffer + 40;

        // TODO check length
        // TODO checksum
    }
    else {
        __android_log_print(ANDROID_LOG_WARN, TAG, "Unknown version %d", version);
        return;
    }

    inet_ntop(version == 4 ? AF_INET : AF_INET6, saddr, source, sizeof(source));
    inet_ntop(version == 4 ? AF_INET : AF_INET6, daddr, dest, sizeof(dest));

    // Get ports & flags
    uint16_t sport = -1;
    uint16_t dport = -1;
    if (protocol == IPPROTO_TCP) {
        struct tcphdr *tcp = payload;

        sport = ntohs(tcp->source);
        dport = ntohs(tcp->dest);

        if (tcp->syn)
            flags[flen++] = 'S';
        if (tcp->ack)
            flags[flen++] = 'A';
        if (tcp->psh)
            flags[flen++] = 'P';
        if (tcp->fin)
            flags[flen++] = 'F';
        if (tcp->fin)
            flags[flen++] = 'R';

        // TODO checksum
    } else if (protocol == IPPROTO_UDP) {
        struct udphdr *udp = payload;

        sport = ntohs(udp->source);
        dport = ntohs(udp->dest);

        // TODO checksum
    }
    flags[flen] = 0;

    // Get uid
    jint uid = -1;
    if (protocol == IPPROTO_TCP || protocol == IPPROTO_UDP) {
        // Sleep 10 ms
        struct timespec tim, tim2;
        tim.tv_sec = 0;
        tim.tv_nsec = 10000000L;
        nanosleep(&tim, &tim2);

        // Lookup uid
        uid = getUid(protocol, version, saddr, sport);
        if (uid < 0 && version == 4) {
            int8_t saddr128[16];
            memset(saddr128, 0, 10);
            saddr128[10] = 0xFF;
            saddr128[11] = 0xFF;
            memcpy(saddr128 + 12, saddr, 4);
            uid = getUid(protocol, 6, saddr128, sport);
        }
    }

    __android_log_print(ANDROID_LOG_DEBUG, TAG,
                        "Packet v%d %s/%u -> %s/%u proto %d flags %s uid %d",
                        version, source, sport, dest, dport, protocol, flags, uid);

    if (protocol == IPPROTO_TCP)
        handle_tcp(env, instance, args, buffer, length);

    // Call back
    jclass cls = (*env)->GetObjectClass(env, instance);
    jmethodID mid = (*env)->GetMethodID(env, cls, "logPacket",
                                        "(ILjava/lang/String;ILjava/lang/String;IILjava/lang/String;IZ)V");
    if (mid == 0)
        __android_log_print(ANDROID_LOG_ERROR, TAG, "logPacket not found");
    else {
        jboolean allowed = 0;
        jstring jsource = (*env)->NewStringUTF(env, source);
        jstring jdest = (*env)->NewStringUTF(env, dest);
        jstring jflags = (*env)->NewStringUTF(env, flags);
        (*env)->CallVoidMethod(env, instance, mid,
                               version,
                               jsource, sport,
                               jdest, dport,
                               protocol, jflags,
                               uid, allowed);
        (*env)->DeleteLocalRef(env, jsource);
        (*env)->DeleteLocalRef(env, jdest);
        (*env)->DeleteLocalRef(env, jflags);

        jthrowable ex = (*env)->ExceptionOccurred(env);
        if (ex) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, ex);
        }
    }
    (*env)->DeleteLocalRef(env, cls);
}

jint getUid(const int protocol, const int version, const void *saddr, const uint16_t sport) {
    char line[250];
    int fields;
    int32_t addr32;
    int8_t addr128[16];
    uint16_t port;
    jint uid = -1;

    // Get proc file name
    char *fn = NULL;
    if (protocol == IPPROTO_TCP)
        fn = (version == 4 ? "/proc/net/tcp" : "/proc/net/tcp6");
    else if (protocol == IPPROTO_UDP)
        fn = (version == 4 ? "/proc/net/udp" : "/proc/net/udp6");
    else
        return -1;

    // Open proc file
    FILE *fd = fopen(fn, "r");
    if (fd == NULL) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Error opening %s", fn);
        return -1;
    }

    // Scan proc file
    int i = 0;
    while (fgets(line, sizeof(line), fd) != NULL) {
        if (i++) {
            if (version == 4)
                fields = sscanf(line,
                                "%*d: %X:%X %*X:%*X %*X %*lX:%*lX %*X:%*X %*X %d %*d %*ld ",
                                &addr32, &port, &uid);
            else
                fields = sscanf(line,
                                "%*d: %8X%8X%8X%8X:%X %*X:%*X %*X %*lX:%*lX %*X:%*X %*X %d %*d %*ld ",
                                addr128, addr128 + 4, addr128 + 8, addr128 + 12, &port, &uid);

            if (fields < 3) {
                __android_log_print(ANDROID_LOG_ERROR, TAG, "Invalid field #%d: %s", fields, line);
                break;
            }

            if (port == sport) {
                if (version == 4) {
                    if (addr32 == *((int32_t *) saddr))
                        return uid;
                }
                else {
                    if (memcmp(addr128, saddr, (size_t) 16) == 0)
                        return uid;
                }
            }
        }
    }

    fclose(fd);

    return -1;
}

// TODO data types
// TODO endianess?
unsigned short checksum(unsigned short *addr, int len) {
    register int sum = 0;
    u_short answer = 0;
    register u_short *w = addr;
    register int nleft = len;

    /*
    * Our algorithm is simple, using a 32-bit accumulator (sum),
    * we add sequential 16-bit words to it, and at the end, fold back
    * all the carry bits from the top 16 bits into the lower 16 bits.
    */

    while (nleft > 1) {
        sum += *w++;
        nleft -= 2;
    }

    /* mop up an odd byte, if necessary */
    if (nleft == 1) {
        *(u_char *) (&answer) = *(u_char *) w;
        sum += answer;
    }

    /* add back carry outs from top 16 bits to low 16 bits */
    sum = (sum >> 16) + (sum & 0xffff); /* add hi 16 to low 16 */
    sum += (sum >> 16); /* add carry */
    answer = ~sum; /* truncate to 16 bits */
    return (answer);
}

void nsleep(const long ns) {
    struct timespec tim, tim2;
    tim.tv_sec = ns / 1000000000L;
    tim.tv_nsec = ns % 1000000000L;
    nanosleep(&tim, &tim2);
}

char hexout[250];

char *hex(const u_int8_t *data, const u_int16_t len) {
    char hex_str[] = "0123456789ABCDEF";

    //char *out;
    //out = (char *) malloc(len * 3 + 1); // TODO free
    hexout[len * 3] = 0;

    for (size_t i = 0; i < len; i++) {
        hexout[i * 3 + 0] = hex_str[(data[i] >> 4) & 0x0F];
        hexout[i * 3 + 1] = hex_str[(data[i]) & 0x0F];
        hexout[i * 3 + 2] = ' ';
    }
    return hexout;
}