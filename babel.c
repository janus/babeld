/*
Copyright (c) 2007, 2008 by Juliusz Chroboczek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <assert.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>

#include "babel.h"
#include "util.h"
#include "net.h"
#include "kernel.h"
#include "network.h"
#include "source.h"
#include "neighbour.h"
#include "route.h"
#include "xroute.h"
#include "message.h"
#include "resend.h"
#include "filter.h"
#include "local.h"

struct timeval now;

unsigned char myid[8];
int debug = 0;

time_t reboot_time;

int idle_time = 320;
int link_detect = 0;
int all_wireless = 0;
int wireless_hello_interval = -1;
int wired_hello_interval = -1;
int idle_hello_interval = -1;
int update_interval = -1;
int do_daemonise = 0;
char *logfile = NULL, *pidfile = "/var/run/babel.pid";

unsigned char *receive_buffer = NULL;
int receive_buffer_size = 0;

const unsigned char zeroes[16] = {0};
const unsigned char ones[16] =
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
     0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

char *state_file = "/var/lib/babel-state";

int protocol_port;
unsigned char protocol_group[16];
int protocol_socket = -1;
int kernel_socket = -1;
static int kernel_routes_changed = 0;
static int kernel_link_changed = 0;
static int kernel_addr_changed = 0;

struct timeval check_neighbours_timeout;

static volatile sig_atomic_t exiting = 0, dumping = 0, changed = 0;

int local_server_socket = -1, local_socket = -1;
int local_server_port = -1;

static int kernel_routes_callback(int changed, void *closure);
static void init_signals(void);
static void dump_tables(FILE *out);
static int reopen_logfile(void);

int
main(int argc, char **argv)
{
    struct sockaddr_in6 sin6;
    int rc, fd, rfd, i;
    time_t expiry_time, source_expiry_time, kernel_dump_time;
    char *config_file = NULL;
    void *vrc;
    unsigned int seed;
    char **arg;
    struct network *net;

    gettime(&now);

    rfd = open("/dev/urandom", O_RDONLY);
    if(rfd < 0) {
        perror("open(random)");
    } else {
        rc = read(rfd, &seed, sizeof(unsigned int));
        if(rc < sizeof(unsigned int)) {
            perror("read(random)");
        }
    }
    seed ^= (now.tv_sec ^ now.tv_usec);
    srandom(seed);

    parse_address("ff02::cca6:c0f9:e182:5373", protocol_group, NULL);
    protocol_port = 8475;

#define SHIFT() do { arg++; } while(0)
#define SHIFTE() do { arg++; if(*arg == NULL) goto syntax; } while(0)

    arg = argv;

    SHIFTE();

    while((*arg)[0] == '-') {
        if(strcmp(*arg, "--") == 0) {
            SHIFTE();
            break;
        } else if(strcmp(*arg, "-m") == 0) {
            SHIFTE();
            rc = parse_address(*arg, protocol_group, NULL);
            if(rc < 0)
                goto syntax;
            if(protocol_group[0] != 0xff) {
                fprintf(stderr,
                        "%s is not a multicast address\n", *arg);
                goto syntax;
            }
            if(protocol_group[1] != 2) {
                fprintf(stderr,
                        "Warning: %s is not a link-local multicast address\n",
                        *arg);
            }
        } else if(strcmp(*arg, "-p") == 0) {
            SHIFTE();
            protocol_port = atoi(*arg);
        } else if(strcmp(*arg, "-h") == 0) {
            SHIFTE();
            wireless_hello_interval = parse_msec(*arg);
            if(wireless_hello_interval <= 0 ||
               wireless_hello_interval > 0xFFFF * 10)
                goto syntax;
        } else if(strcmp(*arg, "-H") == 0) {
            SHIFTE();
            wired_hello_interval = parse_msec(*arg);
            if(wired_hello_interval <= 0 || wired_hello_interval > 0xFFFF * 10)
                goto syntax;
        } else if(strcmp(*arg, "-i") == 0) {
            SHIFTE();
            idle_hello_interval = parse_msec(*arg);
            if(idle_hello_interval <= 0 || idle_hello_interval > 0xFFFF * 10)
                goto syntax;
        } else if(strcmp(*arg, "-u") == 0) {
            SHIFTE();
            update_interval = parse_msec(*arg);
            if(update_interval <= 0 || update_interval > 0xFFFF * 10)
                goto syntax;
        } else if(strcmp(*arg, "-k") == 0) {
            SHIFTE();
            kernel_metric = atoi(*arg);
            if(kernel_metric < 0 || kernel_metric > 0xFFFF)
                goto syntax;
        } else if(strcmp(*arg, "-P") == 0) {
            parasitic = 1;
        } else if(strcmp(*arg, "-s") == 0) {
            split_horizon = 0;
        } else if(strcmp(*arg, "-S") == 0) {
            SHIFTE();
            state_file = *arg;
        } else if(strcmp(*arg, "-d") == 0) {
            SHIFTE();
            debug = atoi(*arg);
#ifndef NO_LOCAL_INTERFACE
        } else if(strcmp(*arg, "-g") == 0) {
            SHIFTE();
            local_server_port = atoi(*arg);
#endif
        } else if(strcmp(*arg, "-l") == 0) {
            link_detect = 1;
        } else if(strcmp(*arg, "-w") == 0) {
            all_wireless = 1;
        } else if(strcmp(*arg, "-t") == 0) {
            SHIFTE();
            export_table = atoi(*arg);
            if(export_table < 0 || export_table > 0xFFFF)
                goto syntax;
        } else if(strcmp(*arg, "-T") == 0) {
            SHIFTE();
            import_table = atoi(*arg);
            if(import_table < 0 || import_table > 0xFFFF)
                goto syntax;
        } else if(strcmp(*arg, "-c") == 0) {
            SHIFTE();
            config_file = *arg;
        } else if(strcmp(*arg, "-C") == 0) {
            SHIFTE();
            rc = parse_config_from_string(*arg);
            if(rc < 0) {
                fprintf(stderr,
                        "Couldn't parse configuration from command line.\n");
                exit(1);
            }
        } else if(strcmp(*arg, "-D") == 0) {
            do_daemonise = 1;
        } else if(strcmp(*arg, "-L") == 0) {
            SHIFTE();
            logfile = *arg;
        } else if(strcmp(*arg, "-I") == 0) {
            SHIFTE();
            pidfile = *arg;
        } else {
            goto syntax;
        }
        SHIFTE();
    }


    if(!config_file) {
        if(access("/etc/babel.conf", R_OK) >= 0)
            config_file = "/etc/babel.conf";
    }
    if(config_file) {
        rc = parse_config_from_file(config_file);
        if(rc < 0) {
            fprintf(stderr,
                    "Couldn't parse configuration from file %s.\n",
                    config_file);
            exit(1);
        }
    }

    rc = finalise_filters();
    if(rc < 0) {
        fprintf(stderr, "Couldn't finalise filters.\n");
        exit(1);
    }

    if(wireless_hello_interval <= 0)
        wireless_hello_interval = 4000;
    wireless_hello_interval = MAX(wireless_hello_interval, 5);

    if(wired_hello_interval <= 0)
        wired_hello_interval = 20000;
    wired_hello_interval = MAX(wired_hello_interval, 5);

    if(update_interval <= 0)
        update_interval =
            MIN(MIN(wireless_hello_interval * 5, wired_hello_interval * 2),
                70000);
    update_interval = MAX(update_interval, 70);

    if(seqno_interval <= 0)
        seqno_interval = MAX(40000, update_interval * 9 / 10);
    seqno_interval = MAX(seqno_interval, 20);

    if(do_daemonise) {
        if(logfile == NULL)
            logfile = "/var/log/babel.log";
    }

    rc = reopen_logfile();
    if(rc < 0) {
        perror("reopen_logfile()");
        exit(1);
    }

    fd = open("/dev/null", O_RDONLY);
    if(fd < 0) {
        perror("open(null)");
        exit(1);
    }

    rc = dup2(fd, 0);
    if(rc < 0) {
        perror("dup2(null, 0)");
        exit(1);
    }

    close(fd);

    if(do_daemonise) {
        rc = daemonise();
        if(rc < 0) {
            perror("daemonise");
            exit(1);
        }
    }

    if(pidfile && pidfile[0] != '\0') {
        int pfd, len;
        char buf[100];

        len = snprintf(buf, 100, "%lu", (unsigned long)getpid());
        if(len < 0 || len >= 100) {
            perror("snprintf(getpid)");
            exit(1);
        }

        pfd = open(pidfile, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if(pfd < 0) {
            char buf[40];
            snprintf(buf, 40, "creat(%s)", pidfile);
            buf[39] = '\0';
            perror(buf);
            exit(1);
        }

        rc = write(pfd, buf, len);
        if(rc < len) {
            perror("write(pidfile)");
            goto fail_pid;
        }

        close(pfd);
    }

    rc = kernel_setup(1);
    if(rc < 0) {
        fprintf(stderr, "kernel_setup failed.\n");
        goto fail_pid;
    }

    rc = kernel_setup_socket(1);
    if(rc < 0) {
        fprintf(stderr, "kernel_setup_socket failed.\n");
        kernel_setup(0);
        goto fail_pid;
    }

    {
        unsigned char dummy[16];
        rc = parse_address(*arg, dummy, NULL);
        if(rc >= 0) {
            fprintf(stderr, "Warning: obsolete router-id given.\n");
            SHIFTE();
        }
    }

    while(*arg) {
        debugf("Adding network %s.\n", *arg);
        vrc = add_network(*arg);
        if(vrc == NULL)
            goto fail;
        SHIFT();
    }

    FOR_ALL_NETS(net) {
        /* net->ifindex is not necessarily valid at this point */
        int ifindex = if_nametoindex(net->ifname);
        if(ifindex > 0) {
            unsigned char eui[8];
            rc = if_eui64(net->ifname, ifindex, eui);
            if(rc < 0)
                continue;
            memcpy(myid, eui, 8);
            goto have_id;
        }
    }

    /* We failed to get a global EUI64 from the interfaces we were given.
       Let's try to find an interface with a MAC address. */
    for(i = 1; i < 256; i++) {
        char buf[IF_NAMESIZE], *ifname;
        unsigned char eui[8];
        ifname = if_indextoname(i, buf);
        if(ifname == NULL)
            continue;
        rc = if_eui64(ifname, i, eui);
        if(rc < 0)
            continue;
        memcpy(myid, eui, 8);
        goto have_id;
    }

    fprintf(stderr,
            "Warning: couldn't find router id -- using random value.\n");
    if(rfd >= 0) {
        rc = read(rfd, myid, 8);
        if(rc < 8) {
            perror("read(random)");
            goto fail;
        }
    } else {
        goto fail;
    }
    /* Clear group and global bits */
    myid[0] &= ~3;

 have_id:
    if(rfd >= 0)
        close(rfd);
    rfd = -1;

    reboot_time = now.tv_sec;
    myseqno = (random() & 0xFFFF);

    fd = open(state_file, O_RDONLY);
    if(fd < 0 && errno != ENOENT)
        perror("open(babel-state)");
    rc = unlink(state_file);
    if(fd >= 0 && rc < 0) {
        perror("unlink(babel-state)");
        /* If we couldn't unlink it, it's probably stale. */
        close(fd);
        fd = -1;
    }
    if(fd >= 0) {
        char buf[100];
        char buf2[100];
        int s;
        long t;
        rc = read(fd, buf, 99);
        if(rc < 0) {
            perror("read(babel-state)");
        } else {
            buf[rc] = '\0';
            rc = sscanf(buf, "%99s %d %ld\n", buf2, &s, &t);
            if(rc == 3 && s >= 0 && s <= 0xFFFF) {
                unsigned char sid[8];
                rc = parse_eui64(buf2, sid);
                if(rc < 0) {
                    fprintf(stderr, "Couldn't parse babel-state.\n");
                } else {
                    struct timeval realnow;
                    debugf("Got %s %d %ld from babel-state.\n",
                           format_address(sid), s, t);
                    gettimeofday(&realnow, NULL);
                    if(memcmp(sid, myid, 8) == 0)
                        myseqno = seqno_plus(s, 1);
                    else
                        fprintf(stderr, "ID mismatch in babel-state.\n");
                    /* Convert realtime into monotonic time. */
                    if(t >= 1176800000L && t <= realnow.tv_sec)
                        reboot_time = now.tv_sec - (realnow.tv_sec - t);
                }
            } else {
                fprintf(stderr, "Couldn't parse babel-state.\n");
            }
        }
        close(fd);
        fd = -1;
    }

    if(reboot_time + silent_time > now.tv_sec)
        fprintf(stderr, "Respecting %ld second silent time.\n",
                (long int)(reboot_time + silent_time - now.tv_sec));

    protocol_socket = babel_socket(protocol_port);
    if(protocol_socket < 0) {
        perror("Couldn't create link local socket");
        goto fail;
    }

#ifndef NO_LOCAL_INTERFACE
    if(local_server_port >= 0) {
        local_server_socket = tcp_server_socket(local_server_port, 1);
        if(local_server_socket < 0) {
            perror("local_server_socket");
            goto fail;
        }
    }
#endif

    init_signals();
    rc = resize_receive_buffer(1500);
    if(rc < 0)
        goto fail;
    check_networks();
    if(receive_buffer == NULL)
        goto fail;

    rc = check_xroutes(0);
    if(rc < 0)
        fprintf(stderr, "Warning: couldn't check exported routes.\n");
    kernel_routes_changed = 0;
    kernel_link_changed = 0;
    kernel_addr_changed = 0;
    kernel_dump_time = now.tv_sec + roughly(30);
    schedule_neighbours_check(5000, 1);
    expiry_time = now.tv_sec + roughly(30);
    source_expiry_time = now.tv_sec + roughly(300);

    /* Make some noise so that others notice us */
    FOR_ALL_NETS(net) {
        if(!net->up)
            continue;
        /* Apply jitter before we send the first message. */
        usleep(roughly(10000));
        gettime(&now);
        send_hello(net);
        send_wildcard_retraction(net);
        send_self_update(net);
        send_request(net, NULL, 0);
        flushupdates(net);
        flushbuf(net);
    }

    debugf("Entering main loop.\n");

    while(1) {
        struct timeval tv;
        fd_set readfds;

        gettime(&now);

        tv = check_neighbours_timeout;
        timeval_min_sec(&tv, expiry_time);
        timeval_min_sec(&tv, source_expiry_time);
        timeval_min_sec(&tv, kernel_dump_time);
        timeval_min(&tv, &resend_time);
        FOR_ALL_NETS(net) {
            if(!net->up)
                continue;
            timeval_min(&tv, &net->flush_timeout);
            timeval_min(&tv, &net->hello_timeout);
            timeval_min(&tv, &net->self_update_timeout);
            timeval_min(&tv, &net->update_timeout);
            timeval_min(&tv, &net->update_flush_timeout);
        }
        timeval_min(&tv, &unicast_flush_timeout);
        FD_ZERO(&readfds);
        if(timeval_compare(&tv, &now) > 0) {
            int maxfd = 0;
            timeval_minus(&tv, &tv, &now);
            FD_SET(protocol_socket, &readfds);
            maxfd = MAX(maxfd, protocol_socket);
            if(kernel_socket < 0) kernel_setup_socket(1);
            if(kernel_socket >= 0) {
                FD_SET(kernel_socket, &readfds);
                maxfd = MAX(maxfd, kernel_socket);
            }
#ifndef NO_LOCAL_INTERFACE
            if(local_socket >= 0) {
                FD_SET(local_socket, &readfds);
                maxfd = MAX(maxfd, local_socket);
            } else if(local_server_socket >= 0) {
                FD_SET(local_server_socket, &readfds);
                maxfd = MAX(maxfd, local_server_socket);
            }
#endif
            rc = select(maxfd + 1, &readfds, NULL, NULL, &tv);
            if(rc < 0) {
                if(errno != EINTR) {
                    perror("select");
                    sleep(1);
                }
                rc = 0;
                FD_ZERO(&readfds);
            }
        }

        gettime(&now);

        if(exiting)
            break;

        if(kernel_socket >= 0 && FD_ISSET(kernel_socket, &readfds))
            kernel_callback(kernel_routes_callback, NULL);

        if(FD_ISSET(protocol_socket, &readfds)) {
            rc = babel_recv(protocol_socket,
                            receive_buffer, receive_buffer_size,
                            (struct sockaddr*)&sin6, sizeof(sin6));
            if(rc < 0) {
                if(errno != EAGAIN && errno != EINTR) {
                    perror("recv");
                    sleep(1);
                }
            } else {
                FOR_ALL_NETS(net) {
                    if(!net->up)
                        continue;
                    if(net->ifindex == sin6.sin6_scope_id) {
                        parse_packet((unsigned char*)&sin6.sin6_addr, net,
                                     receive_buffer, rc);
                        VALGRIND_MAKE_MEM_UNDEFINED(receive_buffer,
                                                    receive_buffer_size);
                        break;
                    }
                }
            }
        }

#ifndef NO_LOCAL_INTERFACE
        if(local_server_socket >= 0 &&
           FD_ISSET(local_server_socket, &readfds)) {
            if(local_socket >= 0) {
                close(local_socket);
                local_socket = -1;
            }
            local_socket = accept(local_server_socket, NULL, NULL);
            if(local_socket < 0) {
                if(errno != EINTR && errno != EAGAIN)
                    perror("accept(local_server_socket)");
            } else {
                local_dump();
            }
        }

        if(local_socket >= 0 && FD_ISSET(local_socket, &readfds)) {
            rc = local_read(local_socket);
            if(rc <= 0) {
                if(rc < 0)
                    perror("read(local_socket)");
                close(local_socket);
                local_socket = -1;
            }
        }
#endif

        if(changed) {
            kernel_dump_time = now.tv_sec;
            check_neighbours_timeout = now;
            expiry_time = now.tv_sec;
            rc = reopen_logfile();
            if(rc < 0) {
                perror("reopen_logfile");
                break;
            }
            changed = 0;
        }

        if(kernel_link_changed || kernel_addr_changed) {
            check_networks();
            kernel_link_changed = 0;
        }

        if(kernel_routes_changed || kernel_addr_changed ||
           now.tv_sec >= kernel_dump_time) {
            rc = check_xroutes(1);
            if(rc < 0)
                fprintf(stderr, "Warning: couldn't check exported routes.\n");
            kernel_routes_changed = kernel_addr_changed = 0;
            if(kernel_socket >= 0)
                kernel_dump_time = now.tv_sec + roughly(300);
            else
                kernel_dump_time = now.tv_sec + roughly(30);
        }

        if(timeval_compare(&check_neighbours_timeout, &now) < 0) {
            int msecs;
            msecs = check_neighbours();
            msecs = MAX(msecs, 10);
            schedule_neighbours_check(msecs, 1);
        }

        if(now.tv_sec >= expiry_time) {
            check_networks();
            expire_routes();
            expire_resend();
            expiry_time = now.tv_sec + roughly(30);
        }

        if(now.tv_sec >= source_expiry_time) {
            expire_sources();
            source_expiry_time = now.tv_sec + roughly(300);
        }

        FOR_ALL_NETS(net) {
            if(!net->up)
                continue;
            if(timeval_compare(&now, &net->hello_timeout) >= 0)
                send_hello(net);
            if(timeval_compare(&now, &net->update_timeout) >= 0)
                send_update(net, 0, NULL, 0);
            if(timeval_compare(&now, &net->self_update_timeout) >= 0)
                send_self_update(net);
            if(timeval_compare(&now, &net->update_flush_timeout) >= 0)
                flushupdates(net);
        }

        if(resend_time.tv_sec != 0) {
            if(timeval_compare(&now, &resend_time) >= 0)
                do_resend();
        }

        if(unicast_flush_timeout.tv_sec != 0) {
            if(timeval_compare(&now, &unicast_flush_timeout) >= 0)
                flush_unicast(1);
        }

        FOR_ALL_NETS(net) {
            if(!net->up)
                continue;
            if(net->flush_timeout.tv_sec != 0) {
                if(timeval_compare(&now, &net->flush_timeout) >= 0)
                    flushbuf(net);
            }
        }

        if(UNLIKELY(debug || dumping)) {
            dump_tables(stdout);
            dumping = 0;
        }
    }

    debugf("Exiting...\n");
    usleep(roughly(10000));
    gettime(&now);

    /* Uninstall and flush all routes. */
    while(numroutes > 0) {
        if(routes[0].installed)
            uninstall_route(&routes[0]);
        /* We need to flush the route so network_up won't reinstall it */
        flush_route(&routes[0]);
    }

    FOR_ALL_NETS(net) {
        if(!net->up)
            continue;
        send_wildcard_retraction(net);
        /* Make sure that we expire quickly from our neighbours'
           association caches. */
        send_hello_noupdate(net, 10);
        flushbuf(net);
        usleep(roughly(1000));
        gettime(&now);
    }
    FOR_ALL_NETS(net) {
        if(!net->up)
            continue;
        /* Make sure they got it. */
        send_wildcard_retraction(net);
        send_hello_noupdate(net, 1);
        flushbuf(net);
        usleep(roughly(10000));
        gettime(&now);
        network_up(net, 0);
    }
    kernel_setup_socket(0);
    kernel_setup(0);

    fd = open(state_file, O_WRONLY | O_TRUNC | O_CREAT, 0644);
    if(fd < 0) {
        perror("creat(babel-state)");
        unlink(state_file);
    } else {
        struct timeval realnow;
        char buf[100];
        gettimeofday(&realnow, NULL);
        rc = snprintf(buf, 100, "%s %d %ld\n",
                      format_eui64(myid), (int)myseqno,
                      (long)realnow.tv_sec);
        if(rc < 0 || rc >= 100) {
            fprintf(stderr, "write(babel-state): overflow.\n");
            unlink(state_file);
        } else {
            rc = write(fd, buf, rc);
            if(rc < 0) {
                perror("write(babel-state)");
                unlink(state_file);
            }
            fsync(fd);
        }
        close(fd);
    }
    if(pidfile)
        unlink(pidfile);
    debugf("Done.\n");
    return 0;

 syntax:
    fprintf(stderr,
            "Syntax: %s "
            "[-m multicast_address] [-p port] [-S state-file]\n"
            "                "
            "[-h hello] [-H wired_hello] [-i idle_hello] [-u update]\n"
            "                "
            "[-k metric] [-s] [-p] [-l] [-w] [-d level] [-g port]\n"
            "                "
            "[-t table] [-T table] [-c file] [-C statement]\n"
            "                "
            "[-D] [-L logfile] [-I pidfile]\n"
            "                "
            "[id] interface...\n",
            argv[0]);
    exit(1);

 fail:
    FOR_ALL_NETS(net) {
        if(!net->up)
            continue;
        network_up(net, 0);
    }
    kernel_setup_socket(0);
    kernel_setup(0);
 fail_pid:
    if(pidfile)
        unlink(pidfile);
    exit(1);
}

/* Schedule a neighbours check after roughly 3/2 times msecs have elapsed. */
void
schedule_neighbours_check(int msecs, int override)
{
    struct timeval timeout;

    timeval_plus_msec(&timeout, &now, roughly(msecs * 3 / 2));
    if(override)
        check_neighbours_timeout = timeout;
    else
        timeval_min(&check_neighbours_timeout, &timeout);
}

int
resize_receive_buffer(int size)
{
    if(size <= receive_buffer_size)
        return 0;

    if(receive_buffer == NULL) {
        receive_buffer = malloc(size);
        if(receive_buffer == NULL) {
            perror("malloc(receive_buffer)");
            return -1;
        }
        receive_buffer_size = size;
    } else {
        unsigned char *new;
        new = realloc(receive_buffer, size);
        if(new == NULL) {
            perror("realloc(receive_buffer)");
            return -1;
        }
        receive_buffer = new;
        receive_buffer_size = size;
    }
    return 1;
}

static void
sigexit(int signo)
{
    exiting = 1;
}

static void
sigdump(int signo)
{
    dumping = 1;
}

static void
sigchanged(int signo)
{
    changed = 1;
}

static void
init_signals(void)
{
    struct sigaction sa;
    sigset_t ss;

    sigemptyset(&ss);
    sa.sa_handler = sigexit;
    sa.sa_mask = ss;
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);

    sigemptyset(&ss);
    sa.sa_handler = sigexit;
    sa.sa_mask = ss;
    sa.sa_flags = 0;
    sigaction(SIGHUP, &sa, NULL);

    sigemptyset(&ss);
    sa.sa_handler = sigexit;
    sa.sa_mask = ss;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    sigemptyset(&ss);
    sa.sa_handler = SIG_IGN;
    sa.sa_mask = ss;
    sa.sa_flags = 0;
    sigaction(SIGPIPE, &sa, NULL);

    sigemptyset(&ss);
    sa.sa_handler = sigdump;
    sa.sa_mask = ss;
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    sigemptyset(&ss);
    sa.sa_handler = sigchanged;
    sa.sa_mask = ss;
    sa.sa_flags = 0;
    sigaction(SIGUSR2, &sa, NULL);

#ifdef SIGINFO
    sigemptyset(&ss);
    sa.sa_handler = sigdump;
    sa.sa_mask = ss;
    sa.sa_flags = 0;
    sigaction(SIGINFO, &sa, NULL);
#endif
}

static void
dump_tables(FILE *out)
{
    struct neighbour *neigh;
    int i;

    fprintf(out, "\n");

    fprintf(out, "My id %s seqno %d\n", format_eui64(myid), myseqno);

    FOR_ALL_NEIGHBOURS(neigh) {
        fprintf(out, "Neighbour %s dev %s reach %04x rxcost %d txcost %d%s.\n",
                format_address(neigh->address),
                neigh->network->ifname,
                neigh->reach,
                neighbour_rxcost(neigh),
                neigh->txcost,
                neigh->network->up ? "" : " (down)");
    }
    for(i = 0; i < numxroutes; i++) {
        fprintf(out, "%s metric %d (exported)\n",
                format_prefix(xroutes[i].prefix, xroutes[i].plen),
                xroutes[i].metric);
    }
    for(i = 0; i < numroutes; i++) {
        const unsigned char *nexthop =
            memcmp(routes[i].nexthop, routes[i].neigh->address, 16) == 0 ?
            NULL : routes[i].nexthop;
        fprintf(out, "%s metric %d refmetric %d id %s seqno %d age %d "
                "via %s neigh %s%s%s%s\n",
                format_prefix(routes[i].src->prefix, routes[i].src->plen),
                routes[i].metric, routes[i].refmetric,
                format_eui64(routes[i].src->id),
                (int)routes[i].seqno,
                (int)(now.tv_sec - routes[i].time),
                routes[i].neigh->network->ifname,
                format_address(routes[i].neigh->address),
                nexthop ? " nexthop " : "",
                nexthop ? format_address(nexthop) : "",
                routes[i].installed ? " (installed)" :
                route_feasible(&routes[i]) ? " (feasible)" : "");
    }
    fflush(out);
}

static int
reopen_logfile()
{
    int lfd, rc;

    if(logfile == NULL)
        return 0;

    lfd = open(logfile, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if(lfd < 0)
        return -1;

    fflush(stdout);
    fflush(stderr);

    rc = dup2(lfd, 1);
    if(rc < 0)
        return -1;

    rc = dup2(lfd, 2);
    if(rc < 0)
        return -1;

    if(lfd > 2)
        close(lfd);

    return 1;
}

static int
kernel_routes_callback(int changed, void *closure)
{
    if (changed & CHANGE_LINK)
        kernel_link_changed = 1;
    if (changed & CHANGE_ADDR)
        kernel_addr_changed = 1;
    if (changed & CHANGE_ROUTE)
        kernel_routes_changed = 1;
    return 1;
}
