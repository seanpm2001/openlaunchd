#include <mach/mach_error.h>
#include <mach/port.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/ucred.h>
#include <sys/fcntl.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/sysctl.h>
#include <sys/sockio.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet6/nd6.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <pthread.h>
#include <paths.h>
#include <pwd.h>

#include "launch.h"
#include "launch_priv.h"
#include "launchd.h"

#include "bootstrap_internal.h"

#define LAUNCHD_MIN_JOB_RUN_TIME 10
#define LAUNCHD_FAILED_EXITS_THRESHOLD 10
#define PID1LAUNCHD_CONF "/etc/launchd.conf"
#define LAUNCHD_CONF ".launchd.conf"
#define LAUNCHCTL_PATH "/bin/launchctl"

extern char **environ;

struct jobcb {
	kq_callback kqjob_callback;
	TAILQ_ENTRY(jobcb) tqe;
	launch_data_t ldj;
	pid_t p;
	struct timeval start_time;
	size_t failed_exits;
	struct conncb *c;
	int checkedin:1, futureflags:31;
};

struct conncb {
	kq_callback kqconn_callback;
	TAILQ_ENTRY(conncb) tqe;
	launch_t conn;
	struct jobcb *j;
};

static TAILQ_HEAD(jobcbhead, jobcb) jobs = TAILQ_HEAD_INITIALIZER(jobs);
static TAILQ_HEAD(conncbhead, conncb) connections = TAILQ_HEAD_INITIALIZER(connections);
static struct jobcb *helperd = NULL;
static int mainkq = 0;
static bool batch_enabled = true;

static launch_data_t load_job(launch_data_t pload);
static launch_data_t get_jobs(const char *which);
static launch_data_t setstdio(int d, launch_data_t o);
static launch_data_t adjust_rlimits(launch_data_t in);
static void batch_job_enable(bool e);
static void do_shutdown(void);

static void listen_callback(void *, struct kevent *);
static void signal_callback(void *, struct kevent *);
static void fs_callback(void *, struct kevent *);
static void simple_zombie_reaper(void *, struct kevent *);
static void mach_callback(void *, struct kevent *);
static void readcfg_callback(void *, struct kevent *);

static kq_callback kqlisten_callback = listen_callback;
static kq_callback kqsignal_callback = signal_callback;
static kq_callback kqfs_callback = fs_callback;
static kq_callback kqmach_callback = mach_callback;
static kq_callback kqreadcfg_callback = readcfg_callback;
kq_callback kqsimple_zombie_reaper = simple_zombie_reaper;

static void job_watch_fds(launch_data_t o, void *cookie);
static void job_ignore_fds(launch_data_t o, void *cookie);
static void job_start(struct jobcb *j);
static void job_stop(struct jobcb *j);
static void job_reap(struct jobcb *j);
static void job_remove(struct jobcb *j);
static void job_callback(void *obj, struct kevent *kev);

static void ipc_open(int fd, struct jobcb *j);
static void ipc_close(struct conncb *c);
static void ipc_callback(void *, struct kevent *);
static void ipc_readmsg(launch_data_t msg, void *context);

#ifdef PID1_REAP_ADOPTED_CHILDREN
static void pid1waitpid(void);
static bool launchd_check_pid(pid_t p);
#endif
static void pid1_magic_init(bool sflag, bool xflag);
static bool launchd_server_init(void);

static void *mach_demand_loop(void *);

static void usage(FILE *where);
static int _fd(int fd);

static void notify_helperd(void);

static void loopback_setup(void);
static void update_lm(void);
static void workaround3048875(int argc, char *argv[]);
static void reload_launchd_config(void);

static pid_t readcfg_pid = 0;
static int thesocket = -1;
static bool debug = false;
static bool verbose = false;
static pthread_t mach_server_loop_thread;
mach_port_t launchd_bootstrap_port = MACH_PORT_NULL;
sigset_t blocked_signals = 0;
static char *pending_stdout = NULL;
static char *pending_stderr = NULL;

int main(int argc, char *argv[])
{
	struct timespec timeout = { 30, 0 };
	struct kevent kev;
	size_t i;
	bool sflag = false, xflag = false;
	int ch, sigigns[] = { SIGHUP, SIGINT, SIGPIPE, SIGALRM,
		SIGTERM, SIGURG, SIGTSTP, SIGTSTP, SIGCONT, /*SIGCHLD,*/
		SIGTTIN, SIGTTOU, SIGIO, SIGXCPU, SIGXFSZ, SIGVTALRM, SIGPROF,
		SIGWINCH, SIGINFO, SIGUSR1, SIGUSR2 };

	if (getpid() == 1)
		workaround3048875(argc, argv);
	
	while ((ch = getopt(argc, argv, "dhsvx")) != -1) {
		switch (ch) {
		case 'd': debug = true;   break;
		case 's': sflag = true;   break;
		case 'x': xflag = true;   break;
		case 'v': verbose = true; break;
		case 'h': usage(stdout);  break;
		default:
			syslog(LOG_WARNING, "ignoring unknown arguments");
			usage(stderr);
			break;
		}
	}

	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
	open(_PATH_DEVNULL, O_RDONLY);
	open(_PATH_DEVNULL, O_WRONLY);
	open(_PATH_DEVNULL, O_WRONLY);

	openlog(getprogname(), LOG_CONS|(getpid() != 1 ? LOG_PID|LOG_PERROR : 0), LOG_LAUNCHD);
	update_lm();

	if ((mainkq = kqueue()) == -1) {
		syslog(LOG_EMERG, "kqueue(): %m");
		exit(EXIT_FAILURE);
	}

	sigemptyset(&blocked_signals);

	for (i = 0; i < (sizeof(sigigns) / sizeof(int)); i++) {
		if (kevent_mod(sigigns[i], EVFILT_SIGNAL, EV_ADD, 0, 0, &kqsignal_callback) == -1)
			syslog(LOG_ERR, "failed to add kevent for signal: %d: %m", sigigns[i]);
		sigaddset(&blocked_signals, sigigns[i]);
		signal(sigigns[i], SIG_IGN);
	}

	/* sigh... ignoring SIGCHLD has side effects: we can't call wait*() */
	if (kevent_mod(SIGCHLD, EVFILT_SIGNAL, EV_ADD, 0, 0, &kqsignal_callback) == -1)
		syslog(LOG_ERR, "failed to add kevent for signal: %d: %m", SIGCHLD);
	
	if (kevent_mod(0, EVFILT_FS, EV_ADD, 0, 0, &kqfs_callback) == -1)
		syslog(LOG_ERR, "kevent_mod(EVFILT_FS, &kqfs_callback): %m");

	if (setsid() == -1)
		syslog(LOG_ERR, "setsid(): %m");

	if (chdir("/") == -1)
		syslog(LOG_ERR, "chdir(\"/\"): %m");

	if (getpid() == 1) {
		pid1_magic_init(sflag, xflag);
	} else if (!launchd_server_init()) {
		exit(EXIT_FAILURE);
	}

	reload_launchd_config();

	for (;;) {
		if (pending_stdout) {
			int fd = open(pending_stdout, O_CREAT|O_APPEND|O_WRONLY, DEFFILEMODE);
			if (fd != -1) {
				dup2(fd, STDOUT_FILENO);
				close(fd);
				free(pending_stdout);
				pending_stdout = NULL;
			}
		}
		if (pending_stderr) {
			int fd = open(pending_stderr, O_CREAT|O_APPEND|O_WRONLY, DEFFILEMODE);
			if (fd != -1) {
				dup2(fd, STDERR_FILENO);
				close(fd);
				free(pending_stderr);
				pending_stderr = NULL;
			}
		}
		if (getpid() == 1 && readcfg_pid == 0)
			init_pre_kevent();
		if (thesocket == -1)
			launchd_server_init();

		switch (kevent(mainkq, NULL, 0, &kev, 1, (TAILQ_EMPTY(&jobs) && getpid() != 1) ? &timeout : NULL)) {
		case -1:
			syslog(LOG_DEBUG, "kevent(): %m");
			break;
		case 1:
			(*((kq_callback *)kev.udata))(kev.udata, &kev);
			break;
		case 0:
			if (TAILQ_EMPTY(&jobs) && getpid() != 1)
				exit(EXIT_SUCCESS);
			else
				syslog(LOG_DEBUG, "kevent(): spurious return with infinite timeout");
			break;
		default:
			syslog(LOG_DEBUG, "unexpected: kevent() returned something != 0, -1 or 1");
			break;
		}

#ifdef PID1_REAP_ADOPTED_CHILDREN
		/* <rdar://problem/3632556> Please automatically reap processes reparented to PID 1 */
		if (getpid() == 1) 
			pid1waitpid();
#endif
	}
}

static void pid1_magic_init(bool sflag, bool xflag)
{
	pthread_attr_t attr;
	int memmib[2] = { CTL_HW, HW_PHYSMEM };
	int mvnmib[2] = { CTL_KERN, KERN_MAXVNODES };
	int hnmib[2] = { CTL_KERN, KERN_HOSTNAME };
	uint64_t mem = 0;
	uint32_t mvn;
	size_t memsz = sizeof(mem);
	int pthr_r;
		
	setpriority(PRIO_PROCESS, 0, -1);

	if (sysctl(memmib, 2, &mem, &memsz, NULL, 0) == -1) {
		syslog(LOG_WARNING, "sysctl(\"hw.physmem\"): %m");
	} else {
		/* The following assignment of mem to itself if the size
		 * of data returned is 32 bits instead of 64 is a clever
		 * C trick to move the 32 bits on big endian systems to
		 * the least significant bytes of the 64 mem variable.
		 *
		 * On little endian systems, this is effectively a no-op.
		 */
		if (memsz == 4)
			mem = *(uint32_t *)&mem;
		mvn = mem / (64 * 1024) + 1024;
		if (sysctl(mvnmib, 2, NULL, NULL, &mvn, sizeof(mvn)) == -1)
			syslog(LOG_WARNING, "sysctl(\"kern.maxvnodes\"): %m");
	}
	if (sysctl(hnmib, 2, NULL, NULL, "localhost", sizeof("localhost")) == -1)
		syslog(LOG_WARNING, "sysctl(\"kern.hostname\"): %m");

	if (setlogin("root") == -1)
		syslog(LOG_ERR, "setlogin(\"root\"): %m");

	loopback_setup();

	if (mount("fdesc", "/dev", MNT_UNION, NULL) == -1)
		syslog(LOG_ERR, "mount(\"%s\", \"%s\", ...): %m", "fdesc", "/dev/");
	if (mount("volfs", "/.vol", MNT_RDONLY, NULL) == -1)
		syslog(LOG_ERR, "mount(\"%s\", \"%s\", ...): %m", "volfs", "/.vol");

	setenv("PATH", _PATH_STDPATH, 1);

	launchd_bootstrap_port = mach_init_init();
	task_set_bootstrap_port(mach_task_self(), launchd_bootstrap_port);
	bootstrap_port = MACH_PORT_NULL;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	pthr_r = pthread_create(&mach_server_loop_thread, &attr, mach_server_loop, NULL);
	if (pthr_r != 0) {
		syslog(LOG_ERR, "pthread_create(mach_server_loop): %s", strerror(pthr_r));
		exit(EXIT_FAILURE);
	}

	pthread_attr_destroy(&attr);

	init_boot(sflag, verbose, xflag);
}


#ifdef PID1_REAP_ADOPTED_CHILDREN
static bool launchd_check_pid(pid_t p)
{
	struct kevent kev;
	struct jobcb *j;

	TAILQ_FOREACH(j, &jobs, tqe) {
		if (j->p == p) {
			EV_SET(&kev, p, EVFILT_PROC, 0, 0, 0, j);
			j->kqjob_callback(j, &kev);
			return true;
		}
	}

	if (p == readcfg_pid) {
		readcfg_callback(NULL, NULL);
		return true;
	}

	return false;
}
#endif

static void launchd_remove_all_jobs(void)
{
	struct jobcb *j;

	while ((j = TAILQ_FIRST(&jobs)))
		job_remove(j);
}

static bool launchd_server_init(void)
{
	struct sockaddr_un sun;
	mode_t oldmask;
	int r, fd = -1, ourdirfd = -1;
	char ourdir[1024];

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;

	snprintf(ourdir, sizeof(ourdir), "%s/%u", LAUNCHD_SOCK_PREFIX, getuid());
	snprintf(sun.sun_path, sizeof(sun.sun_path), "%s/%u/sock", LAUNCHD_SOCK_PREFIX, getuid());

	if (mkdir(LAUNCHD_SOCK_PREFIX, S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH) == -1) {
		if (errno == EROFS)
			return false;
		if (errno != EEXIST) {
			syslog(LOG_ERR, "mkdir(\"%s\"): %m", LAUNCHD_SOCK_PREFIX);
			exit(EXIT_FAILURE);
		}
	}

	unlink(ourdir);
	if (mkdir(ourdir, S_IRWXU) == -1) {
		if (errno == EROFS) {
			return false;
		} else if (errno == EEXIST) {
			struct stat sb;
			stat(ourdir, &sb);
			if (!S_ISDIR(sb.st_mode))
				return false;
		} else {
			syslog(LOG_ERR, "mkdir(\"%s\"): %m", ourdir);
			exit(EXIT_FAILURE);
		}
	}
	if (chown(ourdir, getuid(), getgid()) == -1)
		syslog(LOG_WARNING, "chown(\"%s\"): %m", ourdir);

	ourdirfd = _fd(open(ourdir, O_RDONLY));
	if (ourdirfd == -1) {
		syslog(LOG_ERR, "open(\"%s\"): %m", ourdir);
		exit(EXIT_FAILURE);
	}

	if (flock(ourdirfd, LOCK_EX|LOCK_NB) == -1) {
		if (errno == EWOULDBLOCK) {
			exit(EXIT_SUCCESS);
		} else {
			syslog(LOG_ERR, "flock(\"%s\"): %m", ourdir);
			exit(EXIT_FAILURE);
		}
	}

	if (unlink(sun.sun_path) == -1 && errno != ENOENT) {
		if (errno != EROFS)
			syslog(LOG_ERR, "unlink(\"thesocket\"): %m");
		goto out_bad;
	}
	if ((fd = _fd(socket(AF_UNIX, SOCK_STREAM, 0))) == -1) {
		syslog(LOG_ERR, "socket(\"thesocket\"): %m");
		goto out_bad;
	}
	oldmask = umask(077);
	r = bind(fd, (struct sockaddr *)&sun, sizeof(sun));
	umask(oldmask);
	if (r == -1) {
		if (errno != EROFS)
			syslog(LOG_ERR, "bind(\"thesocket\"): %m");
		goto out_bad;
	}
	if (chown(sun.sun_path, getuid(), getgid()) == -1)
		syslog(LOG_WARNING, "chown(\"thesocket\"): %m");
	if (listen(fd, SOMAXCONN) == -1) {
		syslog(LOG_ERR, "listen(\"thesocket\"): %m");
		goto out_bad;
	}

	if (kevent_mod(fd, EVFILT_READ, EV_ADD, 0, 0, &kqlisten_callback) == -1) {
		syslog(LOG_ERR, "kevent_mod(\"thesocket\", EVFILT_READ): %m");
		goto out_bad;
	}

	thesocket = fd;
	setgid(getgid());
	setuid(getuid());

	return true;
out_bad:
	if (fd != -1)
		close(fd);
	if (ourdirfd != -1)
		close(ourdirfd);
	return false;
}

static long long job_get_integer(launch_data_t j, const char *key)
{
	launch_data_t t = launch_data_dict_lookup(j, key);
	if (t)
		return launch_data_get_integer(t);
	else
		return 0;
}

static const char *job_get_string(launch_data_t j, const char *key)
{
	launch_data_t t = launch_data_dict_lookup(j, key);
	if (t)
		return launch_data_get_string(t);
	else
		return NULL;
}

static const char *job_get_argv0(launch_data_t j)
{
	launch_data_t tmpi, tmp = launch_data_dict_lookup(j, LAUNCH_JOBKEY_PROGRAM);

	if (tmp) {
		return launch_data_get_string(tmp);
	} else {
		tmp = launch_data_dict_lookup(j, LAUNCH_JOBKEY_PROGRAMARGUMENTS);
		if (tmp) {
			tmpi = launch_data_array_get_index(tmp, 0);
			if (tmpi)
				return launch_data_get_string(tmpi);
		}
		return NULL;
	}
}

static bool job_get_bool(launch_data_t j, const char *key)
{
	launch_data_t t = launch_data_dict_lookup(j, key);
	if (t)
		return launch_data_get_bool(t);
	else
		return false;
}

static void ipc_open(int fd, struct jobcb *j)
{
	struct conncb *c = calloc(1, sizeof(struct conncb));

	fcntl(fd, F_SETFL, O_NONBLOCK);

	c->kqconn_callback = ipc_callback;
	c->conn = launchd_fdopen(fd);
	c->j = j;
	if (j)
		j->c = c;
	TAILQ_INSERT_TAIL(&connections, c, tqe);
	kevent_mod(fd, EVFILT_READ, EV_ADD, 0, 0, &c->kqconn_callback);
}

static void simple_zombie_reaper(void *obj __attribute__((unused)), struct kevent *kev)
{
	waitpid(kev->ident, NULL, 0);
}

static void listen_callback(void *obj __attribute__((unused)), struct kevent *kev)
{
	struct sockaddr_un sun;
	socklen_t sl = sizeof(sun);
	int cfd;

	if ((cfd = _fd(accept(kev->ident, (struct sockaddr *)&sun, &sl))) == -1) {
		return;
	}

	ipc_open(cfd, NULL);
}

static void ipc_callback(void *obj, struct kevent *kev)
{
	struct conncb *c = obj;
	int r;
	
	if (kev->filter == EVFILT_READ) {
		if (launchd_msg_recv(c->conn, ipc_readmsg, c) == -1 && errno != EAGAIN) {
			if (errno != ECONNRESET)
				syslog(LOG_DEBUG, "%s(): recv: %m", __func__);
			ipc_close(c);
		}
	} else if (kev->filter == EVFILT_WRITE) {
		r = launchd_msg_send(c->conn, NULL);
		if (r == -1) {
			if (errno != EAGAIN) {
				syslog(LOG_DEBUG, "%s(): send: %m", __func__);
				ipc_close(c);
			}
		} else if (r == 0) {
			kevent_mod(launchd_getfd(c->conn), EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
		}
	} else {
		syslog(LOG_DEBUG, "%s(): unknown filter type!", __func__);
		ipc_close(c);
	}
}

static void set_user_env(launch_data_t obj, const char *key, void *context __attribute__((unused)))
{
	setenv(key, launch_data_get_string(obj), 1);
}

static void launch_data_close_fds(launch_data_t o)
{
	size_t i;

	switch (launch_data_get_type(o)) {
	case LAUNCH_DATA_DICTIONARY:
		launch_data_dict_iterate(o, (void (*)(launch_data_t, const char *, void *))launch_data_close_fds, NULL);
		break;
	case LAUNCH_DATA_ARRAY:
		for (i = 0; i < launch_data_array_get_count(o); i++)
			launch_data_close_fds(launch_data_array_get_index(o, i));
		break;
	case LAUNCH_DATA_FD:
		if (launch_data_get_fd(o) != -1)
			close(launch_data_get_fd(o));
		break;
	default:
		break;
	}
}

static void launch_data_revoke_fds(launch_data_t o)
{
	size_t i;

	switch (launch_data_get_type(o)) {
	case LAUNCH_DATA_DICTIONARY:
		launch_data_dict_iterate(o, (void (*)(launch_data_t, const char *, void *))launch_data_revoke_fds, NULL);
		break;
	case LAUNCH_DATA_ARRAY:
		for (i = 0; i < launch_data_array_get_count(o); i++)
			launch_data_revoke_fds(launch_data_array_get_index(o, i));
		break;
	case LAUNCH_DATA_FD:
		launch_data_set_fd(o, -1);
		break;
	default:
		break;
	}
}

static void job_ignore_fds_dict(launch_data_t o, const char *k __attribute__((unused)), void *cookie)
{
	job_ignore_fds(o, cookie);
}

static void job_ignore_fds(launch_data_t o, void *cookie)
{
	size_t i;

	switch (launch_data_get_type(o)) {
	case LAUNCH_DATA_DICTIONARY:
		launch_data_dict_iterate(o, job_ignore_fds_dict, cookie);
		break;
	case LAUNCH_DATA_ARRAY:
		for (i = 0; i < launch_data_array_get_count(o); i++)
			job_ignore_fds(launch_data_array_get_index(o, i), cookie);
		break;
	case LAUNCH_DATA_FD:
		kevent_mod(launch_data_get_fd(o), EVFILT_READ, EV_DELETE, 0, 0, cookie);
		break;
	default:
		break;
	}
}

static void job_watch_fds_dict(launch_data_t o, const char *k __attribute__((unused)), void *cookie)
{
	job_watch_fds(o, cookie);
}

static void job_watch_fds(launch_data_t o, void *cookie)
{
	size_t i;

	switch (launch_data_get_type(o)) {
	case LAUNCH_DATA_DICTIONARY:
		launch_data_dict_iterate(o, job_watch_fds_dict, cookie);
		break;
	case LAUNCH_DATA_ARRAY:
		for (i = 0; i < launch_data_array_get_count(o); i++)
			job_watch_fds(launch_data_array_get_index(o, i), cookie);
		break;
	case LAUNCH_DATA_FD:
		kevent_mod(launch_data_get_fd(o), EVFILT_READ, EV_ADD, 0, 0, cookie);
		break;
	default:
		break;
	}
}

static void job_stop(struct jobcb *j)
{
	if (j->p)
		kill(j->p, SIGTERM);
}

static void job_remove(struct jobcb *j)
{
	TAILQ_REMOVE(&jobs, j, tqe);
	if (j->p) {
		if (kevent_mod(j->p, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, &kqsimple_zombie_reaper) == -1) {
			job_reap(j);
		} else {
			job_stop(j);
		}
	}
	launch_data_close_fds(j->ldj);
	launch_data_free(j->ldj);
	free(j);
}

static void ipc_readmsg(launch_data_t msg, void *context)
{
	struct conncb *c = context;
	struct jobcb *j;
	launch_data_t pload, tmp, resp = NULL;
	size_t i;

	if ((LAUNCH_DATA_DICTIONARY == launch_data_get_type(msg)) &&
			(tmp = launch_data_dict_lookup(msg, LAUNCH_KEY_STARTJOB))) {
		resp = launch_data_alloc(LAUNCH_DATA_STRING);
		TAILQ_FOREACH(j, &jobs, tqe) {
			if (!strcmp(job_get_string(j->ldj, LAUNCH_JOBKEY_LABEL), launch_data_get_string(tmp))) {
				job_start(j);
				launch_data_set_string(resp, LAUNCH_RESPONSE_SUCCESS);
				goto out;
			}
		}
		launch_data_set_string(resp, LAUNCH_RESPONSE_JOBNOTFOUND);
	} else if ((LAUNCH_DATA_DICTIONARY == launch_data_get_type(msg)) &&
			(tmp = launch_data_dict_lookup(msg, LAUNCH_KEY_STOPJOB))) {
		resp = launch_data_alloc(LAUNCH_DATA_STRING);
		TAILQ_FOREACH(j, &jobs, tqe) {
			if (!strcmp(job_get_string(j->ldj, LAUNCH_JOBKEY_LABEL), launch_data_get_string(tmp))) {
				job_stop(j);
				launch_data_set_string(resp, LAUNCH_RESPONSE_SUCCESS);
				goto out;
			}
		}
		launch_data_set_string(resp, LAUNCH_RESPONSE_JOBNOTFOUND);
	} else if ((LAUNCH_DATA_DICTIONARY == launch_data_get_type(msg)) &&
			(tmp = launch_data_dict_lookup(msg, LAUNCH_KEY_REMOVEJOB))) {
		resp = launch_data_alloc(LAUNCH_DATA_STRING);
		TAILQ_FOREACH(j, &jobs, tqe) {
			if (!strcmp(job_get_string(j->ldj, LAUNCH_JOBKEY_LABEL), launch_data_get_string(tmp))) {
				if (!strcmp(launch_data_get_string(tmp), HELPERD))
					helperd = NULL;
				job_remove(j);
				launch_data_set_string(resp, LAUNCH_RESPONSE_SUCCESS);
				notify_helperd();
				goto out;
			}
		}
		launch_data_set_string(resp, LAUNCH_RESPONSE_JOBNOTFOUND);
	} else if ((LAUNCH_DATA_DICTIONARY == launch_data_get_type(msg)) &&
			(pload = launch_data_dict_lookup(msg, LAUNCH_KEY_SUBMITJOB))) {
		if (launch_data_get_type(pload) == LAUNCH_DATA_ARRAY) {
			resp = launch_data_alloc(LAUNCH_DATA_ARRAY);
			for (i = 0; i < launch_data_array_get_count(pload); i++) {
				tmp = load_job(launch_data_array_get_index(pload, i));
				launch_data_array_set_index(resp, tmp, i);
			}
		} else {
			resp = load_job(pload);
		}
	} else if ((LAUNCH_DATA_DICTIONARY == launch_data_get_type(msg)) &&
			(pload = launch_data_dict_lookup(msg, LAUNCH_KEY_UNSETUSERENVIRONMENT))) {
		unsetenv(launch_data_get_string(pload));
		resp = launch_data_alloc(LAUNCH_DATA_STRING);
		launch_data_set_string(resp, LAUNCH_RESPONSE_SUCCESS);
	} else if ((LAUNCH_DATA_STRING == launch_data_get_type(msg)) &&
			!strcmp(launch_data_get_string(msg), LAUNCH_KEY_GETUSERENVIRONMENT)) {
		char **tmpenviron = environ;
		resp = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
		for (; *tmpenviron; tmpenviron++) {
			char envkey[1024];
			launch_data_t s = launch_data_alloc(LAUNCH_DATA_STRING);
			launch_data_set_string(s, strchr(*tmpenviron, '=') + 1);
			strncpy(envkey, *tmpenviron, sizeof(envkey));
			*(strchr(envkey, '=')) = '\0';
			launch_data_dict_insert(resp, s, envkey);
		}
	} else if ((LAUNCH_DATA_DICTIONARY == launch_data_get_type(msg)) &&
			(pload = launch_data_dict_lookup(msg, LAUNCH_KEY_SETUSERENVIRONMENT))) {
		launch_data_dict_iterate(pload, set_user_env, NULL);
		resp = launch_data_alloc(LAUNCH_DATA_STRING);
		launch_data_set_string(resp, LAUNCH_RESPONSE_SUCCESS);
	} else if ((LAUNCH_DATA_STRING == launch_data_get_type(msg)) &&
			!strcmp(launch_data_get_string(msg), LAUNCH_KEY_CHECKIN)) {
		if (c->j) {
			resp = launch_data_copy(c->j->ldj);
			c->j->checkedin = true;
		} else {
			resp = launch_data_alloc(LAUNCH_DATA_STRING);
			launch_data_set_string(resp, LAUNCH_RESPONSE_NOTRUNNINGFROMLAUNCHD);
		}
	} else if ((LAUNCH_DATA_STRING == launch_data_get_type(msg)) &&
			!strcmp(launch_data_get_string(msg), LAUNCH_KEY_RELOADTTYS)) {
		update_ttys();
		resp = launch_data_new_string(LAUNCH_RESPONSE_SUCCESS);
	} else if ((LAUNCH_DATA_STRING == launch_data_get_type(msg)) &&
			!strcmp(launch_data_get_string(msg), LAUNCH_KEY_SHUTDOWN)) {
		do_shutdown();
		resp = launch_data_new_string(LAUNCH_RESPONSE_SUCCESS);
	} else if ((LAUNCH_DATA_STRING == launch_data_get_type(msg)) &&
			!strcmp(launch_data_get_string(msg), LAUNCH_KEY_GETJOBS)) {
		resp = get_jobs(NULL);
		launch_data_revoke_fds(resp);
	} else if ((LAUNCH_DATA_STRING == launch_data_get_type(msg)) &&
			!strcmp(launch_data_get_string(msg), LAUNCH_KEY_GETRESOURCELIMITS)) {
		resp = adjust_rlimits(NULL);
	} else if ((LAUNCH_DATA_DICTIONARY == launch_data_get_type(msg)) &&
			(tmp = launch_data_dict_lookup(msg, LAUNCH_KEY_SETRESOURCELIMITS))) {
		resp = adjust_rlimits(tmp);
	} else if ((LAUNCH_DATA_DICTIONARY == launch_data_get_type(msg)) &&
			(tmp = launch_data_dict_lookup(msg, LAUNCH_KEY_GETJOB))) {
		resp = get_jobs(launch_data_get_string(tmp));
		launch_data_revoke_fds(resp);
	} else if ((LAUNCH_DATA_DICTIONARY == launch_data_get_type(msg)) &&
			(tmp = launch_data_dict_lookup(msg, LAUNCH_KEY_GETJOBWITHHANDLES))) {
		resp = get_jobs(launch_data_get_string(tmp));
	} else if ((LAUNCH_DATA_DICTIONARY == launch_data_get_type(msg)) &&
			(tmp = launch_data_dict_lookup(msg, LAUNCH_KEY_SETUMASK))) {
		resp = launch_data_new_integer(umask(launch_data_get_integer(tmp)));
	} else if ((LAUNCH_DATA_STRING == launch_data_get_type(msg)) &&
			!strcmp(launch_data_get_string(msg), LAUNCH_KEY_GETUMASK)) {
		mode_t oldmask = umask(0);
		resp = launch_data_new_integer(oldmask);
		umask(oldmask);
	} else if ((LAUNCH_DATA_DICTIONARY == launch_data_get_type(msg)) &&
			(tmp = launch_data_dict_lookup(msg, LAUNCH_KEY_SETSTDOUT))) {
		resp = setstdio(STDOUT_FILENO, tmp);
	} else if ((LAUNCH_DATA_DICTIONARY == launch_data_get_type(msg)) &&
			(tmp = launch_data_dict_lookup(msg, LAUNCH_KEY_SETSTDERR))) {
		resp = setstdio(STDERR_FILENO, tmp);
	} else if ((LAUNCH_DATA_DICTIONARY == launch_data_get_type(msg)) &&
			(tmp = launch_data_dict_lookup(msg, LAUNCH_KEY_BATCHCONTROL))) {
		batch_job_enable(launch_data_get_bool(tmp));
		resp = launch_data_alloc(LAUNCH_DATA_STRING);
		launch_data_set_string(resp, LAUNCH_RESPONSE_SUCCESS);
	} else if ((LAUNCH_DATA_STRING == launch_data_get_type(msg)) &&
			!strcmp(launch_data_get_string(msg), LAUNCH_KEY_BATCHQUERY)) {
		resp = launch_data_alloc(LAUNCH_DATA_BOOL);
		launch_data_set_bool(resp, batch_enabled);
	} else {	
		resp = launch_data_alloc(LAUNCH_DATA_STRING);
		launch_data_set_string(resp, LAUNCH_RESPONSE_UNKNOWNCOMMAND);
	}

	launch_data_close_fds(msg);
out:
	if (launchd_msg_send(c->conn, resp) == -1) {
		if (errno == EAGAIN) {
			kevent_mod(launchd_getfd(c->conn), EVFILT_WRITE, EV_ADD, 0, 0, &c->kqconn_callback);
		} else {
			syslog(LOG_DEBUG, "launchd_msg_send() == -1: %m");
			ipc_close(c);
		}
	}
	launch_data_free(resp);
}

static launch_data_t setstdio(int d, launch_data_t o)
{
	launch_data_t resp = launch_data_new_string(LAUNCH_RESPONSE_SUCCESS);

	if (launch_data_get_type(o) == LAUNCH_DATA_STRING) {
		char **where = &pending_stderr;
		if (d == STDOUT_FILENO)
			where = &pending_stdout;
		if (*where)
			free(*where);
		*where = strdup(launch_data_get_string(o));
	} else if (launch_data_get_type(o) == LAUNCH_DATA_FD) {
		dup2(launch_data_get_fd(o), d);
	} else {
		launch_data_set_string(resp, LAUNCH_RESPONSE_UNKNOWNCOMMAND);
	}

	return resp;
}

static void batch_job_enable(bool e)
{
	if (e) {
		batch_enabled = true;
		if (helperd && helperd->p)
			kill(helperd->p, SIGCONT);
	} else {
		batch_enabled = false;
		if (helperd && helperd->p)
			kill(helperd->p, SIGSTOP);
	}
}

static launch_data_t load_job(launch_data_t pload)
{
	launch_data_t label, tmp, resp;
	struct jobcb *j;

	if ((label = launch_data_dict_lookup(pload, LAUNCH_JOBKEY_LABEL))) {
		TAILQ_FOREACH(j, &jobs, tqe) {
			if (!strcmp(job_get_string(j->ldj, LAUNCH_JOBKEY_LABEL), launch_data_get_string(label))) {
				resp = launch_data_alloc(LAUNCH_DATA_STRING);
				launch_data_set_string(resp, LAUNCH_RESPONSE_JOBEXISTS);
				goto out;
			}
		}
	} else {
		resp = launch_data_alloc(LAUNCH_DATA_STRING);
		launch_data_set_string(resp, LAUNCH_RESPONSE_LABELMISSING);
		goto out;
	}
	if (launch_data_dict_lookup(pload, LAUNCH_JOBKEY_PROGRAMARGUMENTS) == NULL) {
		resp = launch_data_alloc(LAUNCH_DATA_STRING);
		launch_data_set_string(resp, LAUNCH_RESPONSE_PROGRAMARGUMENTSMISSING);
		goto out;
	}

	j = calloc(1, sizeof(struct jobcb));
	j->ldj = launch_data_copy(pload);
	launch_data_revoke_fds(pload);
	j->kqjob_callback = job_callback;


	if (launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_ONDEMAND) == NULL) {
		tmp = launch_data_alloc(LAUNCH_DATA_BOOL);
		launch_data_set_bool(tmp, true);
		launch_data_dict_insert(j->ldj, tmp, LAUNCH_JOBKEY_ONDEMAND);
	}

	if (launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_SERVICEIPC) == NULL) {
		tmp = launch_data_alloc(LAUNCH_DATA_BOOL);
		launch_data_set_bool(tmp, true);
		launch_data_dict_insert(j->ldj, tmp, LAUNCH_JOBKEY_SERVICEIPC);
	}

	TAILQ_INSERT_TAIL(&jobs, j, tqe);

	if (job_get_bool(j->ldj, LAUNCH_JOBKEY_ONDEMAND))
		job_watch_fds(j->ldj, &j->kqjob_callback);
	else
		job_start(j);

	if (!strcmp(launch_data_get_string(label), HELPERD))
		helperd = j;

	resp = launch_data_alloc(LAUNCH_DATA_STRING);
	launch_data_set_string(resp, LAUNCH_RESPONSE_SUCCESS);
	notify_helperd();
out:
	return resp;
}

static launch_data_t get_jobs(const char *which)
{
	struct jobcb *j;
	launch_data_t tmp, resp = NULL;

	if (which) {
		TAILQ_FOREACH(j, &jobs, tqe) {
			if (!strcmp(which, job_get_string(j->ldj, LAUNCH_JOBKEY_LABEL)))
				resp = launch_data_copy(j->ldj);
		}
		if (resp == NULL)
			resp = launch_data_new_string(LAUNCH_RESPONSE_JOBNOTFOUND);
	} else {
		resp = launch_data_alloc(LAUNCH_DATA_DICTIONARY);

		TAILQ_FOREACH(j, &jobs, tqe) {
			tmp = launch_data_copy(j->ldj);
			launch_data_dict_insert(resp, tmp, job_get_string(tmp, LAUNCH_JOBKEY_LABEL));
		}
	}

	return resp;
}

static void usage(FILE *where)
{
	fprintf(where, "%s:\n", getprogname());
	fprintf(where, "\t-d\tdebug mode\n");
	fprintf(where, "\t-h\tthis usage statement\n");

	if (where == stdout)
		exit(EXIT_SUCCESS);
}

static void **machcbtable = NULL;
static size_t machcbtable_cnt = 0;
static int machcbreadfd = -1;
static int machcbwritefd = -1;
static mach_port_t mach_demand_port_set = MACH_PORT_NULL;
static pthread_t mach_demand_thread;

static void mach_callback(void *obj __attribute__((unused)), struct kevent *kev __attribute__((unused)))
{
	struct kevent mkev;
	mach_port_t mp;

	read(machcbreadfd, &mp, sizeof(mp));

	EV_SET(&mkev, mp, EVFILT_MACHPORT, 0, 0, 0, machcbtable[MACH_PORT_INDEX(mp)]);

	(*((kq_callback *)mkev.udata))(mkev.udata, &mkev);
}

int kevent_mod(uintptr_t ident, short filter, u_short flags, u_int fflags, intptr_t data, void *udata)
{
	struct kevent kev;
	kern_return_t kr;
	pthread_attr_t attr;
	int pthr_r, pfds[2];

	if (filter != EVFILT_MACHPORT) {
#ifdef PID1_REAP_ADOPTED_CHILDREN
		if (filter == EVFILT_PROC && getpid() == 1)
			return 0;
#endif
		EV_SET(&kev, ident, filter, flags, fflags, data, udata);
		return kevent(mainkq, &kev, 1, NULL, 0, NULL);
	}

	if (machcbtable == NULL) {
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

		pthr_r = pthread_create(&mach_demand_thread, &attr, mach_demand_loop, NULL);
		if (pthr_r != 0) {
			syslog(LOG_ERR, "pthread_create(mach_demand_loop): %s", strerror(pthr_r));
			exit(EXIT_FAILURE);
		}

		pthread_attr_destroy(&attr);

		machcbtable = malloc(0);
		pipe(pfds);
		machcbwritefd = _fd(pfds[1]);
		machcbreadfd = _fd(pfds[0]);
		kevent_mod(machcbreadfd, EVFILT_READ, EV_ADD, 0, 0, &kqmach_callback);
		kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &mach_demand_port_set);
		if (kr != KERN_SUCCESS) {
			syslog(LOG_ERR, "mach_port_allocate(demand_port_set): %s", mach_error_string(kr));
			exit(EXIT_FAILURE);
		}
	}

	if (flags & EV_ADD) {
		kr = mach_port_move_member(mach_task_self(), ident, mach_demand_port_set);
		if (kr != KERN_SUCCESS) {
			syslog(LOG_ERR, "mach_port_move_member(): %s", mach_error_string(kr));
			exit(EXIT_FAILURE);
		}

		if (MACH_PORT_INDEX(ident) > machcbtable_cnt)
			machcbtable = realloc(machcbtable, MACH_PORT_INDEX(ident) * sizeof(void *));

		machcbtable[MACH_PORT_INDEX(ident)] = udata;
	} else if (flags & EV_DELETE) {
		kr = mach_port_move_member(mach_task_self(), ident, MACH_PORT_NULL);
		if (kr != KERN_SUCCESS) {
			syslog(LOG_ERR, "mach_port_move_member(): %s", mach_error_string(kr));
			exit(EXIT_FAILURE);
		}
	} else {
		syslog(LOG_DEBUG, "kevent_mod(EVFILT_MACHPORT) with flags: %d", flags);
		errno = EINVAL;
		return -1;
	}

	return 0;
}

static int _fd(int fd)
{
	if (fd >= 0)
		fcntl(fd, F_SETFD, 1);
	return fd;
}

static void ipc_close(struct conncb *c)
{
	batch_job_enable(true);

	TAILQ_REMOVE(&connections, c, tqe);
	launchd_close(c->conn);
	free(c);
}

static void setup_job_env(launch_data_t obj, const char *key, void *context __attribute__((unused)))
{
	if (LAUNCH_DATA_STRING == launch_data_get_type(obj))
		setenv(key, launch_data_get_string(obj), 1);
}

static void job_reap(struct jobcb *j)
{
	bool bad_exit = false;
	int status;

#ifdef PID1_REAP_ADOPTED_CHILDREN
	if (getpid() == 1)
		status = pid1_child_exit_status;
	else
#endif
		waitpid(j->p, &status, 0);

	if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
		syslog(LOG_WARNING, "%s[%d] exited with exit code %d",
				job_get_argv0(j->ldj), j->p, WEXITSTATUS(status));
		bad_exit = true;
	}

	if (WIFSIGNALED(status)) {
		int s = WTERMSIG(status);
		if (s != SIGKILL && s != SIGTERM) {
			syslog(LOG_WARNING, "%s[%d] exited abnormally: %s",
					job_get_argv0(j->ldj), j->p, strsignal(WTERMSIG(status)));
			bad_exit = true;
		}
	}

	if (bad_exit)
		j->failed_exits++;
	else
		j->failed_exits = 0;

	j->c = NULL;
	j->p = 0;
	j->checkedin = false;
}

static void job_callback(void *obj, struct kevent *kev)
{
	struct jobcb *j = obj;

	if (kev->filter == EVFILT_PROC) {

		if (job_get_bool(j->ldj, LAUNCH_JOBKEY_SERVICEIPC) && !j->checkedin) {
			syslog(LOG_WARNING, "%s failed to checkin, removing job", job_get_argv0(j->ldj));
			job_remove(j);
			return;
		}

		job_reap(j);

		if (j->failed_exits > LAUNCHD_FAILED_EXITS_THRESHOLD) {
			syslog(LOG_NOTICE, "Too many failures in a row with %s, removing job", job_get_argv0(j->ldj));
			job_remove(j);
			return;
		}

		if (job_get_bool(j->ldj, LAUNCH_JOBKEY_ONDEMAND)) {
			job_watch_fds(j->ldj, &j->kqjob_callback);
			return;
		}

		if (j == helperd && !batch_enabled)
			return;
	}

	job_start(j);
}

static void job_start(struct jobcb *j)
{
	char nbuf[64];
	pid_t c;
	int spair[2];
	const char **argv;
	bool sipc;
	struct timeval tvd, last_start_time = j->start_time;

	if (j->p)
		return;

	if ((sipc = job_get_bool(j->ldj, LAUNCH_JOBKEY_SERVICEIPC)))
		socketpair(AF_UNIX, SOCK_STREAM, 0, spair);

	gettimeofday(&j->start_time, NULL);
	timersub(&j->start_time, &last_start_time, &tvd);

	if (tvd.tv_sec >= LAUNCHD_MIN_JOB_RUN_TIME) {
		/* If the daemon lived long enough, let's reward it.
		 * This lets infrequent bugs not cause the daemon to removed */
		j->failed_exits = 0;
	}
	
	if ((c = fork_with_bootstrap_port(launchd_bootstrap_port)) == -1) {
		syslog(LOG_WARNING, "fork(): %m");
		if (sipc) {
			close(spair[0]);
			close(spair[1]);
		}
		return;
	} else if (c == 0) {
		launch_data_t ldpa = launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_PROGRAMARGUMENTS);
		launch_data_t srl = launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_SOFTRESOURCELIMITS);
		launch_data_t hrl = launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_HARDRESOURCELIMITS);
		size_t i, argv_cnt;
		const char *a0;
		const struct {
			const char *key;
			int val;
		} limits[] = {
			{ LAUNCH_JOBKEY_RESOURCELIMIT_CORE,    RLIMIT_CORE    },
			{ LAUNCH_JOBKEY_RESOURCELIMIT_CPU,     RLIMIT_CPU     },
			{ LAUNCH_JOBKEY_RESOURCELIMIT_DATA,    RLIMIT_DATA    },
			{ LAUNCH_JOBKEY_RESOURCELIMIT_FSIZE,   RLIMIT_FSIZE   },
			{ LAUNCH_JOBKEY_RESOURCELIMIT_MEMLOCK, RLIMIT_MEMLOCK },
			{ LAUNCH_JOBKEY_RESOURCELIMIT_NOFILE,  RLIMIT_NOFILE  },
			{ LAUNCH_JOBKEY_RESOURCELIMIT_NPROC,   RLIMIT_NPROC   },
			{ LAUNCH_JOBKEY_RESOURCELIMIT_RSS,     RLIMIT_RSS     },
			{ LAUNCH_JOBKEY_RESOURCELIMIT_STACK,   RLIMIT_STACK   },
		};

		argv_cnt = launch_data_array_get_count(ldpa);
		argv = alloca((argv_cnt + 1) * sizeof(char *));
		for (i = 0; i < argv_cnt; i++)
			argv[i] = launch_data_get_string(launch_data_array_get_index(ldpa, i));
		argv[argv_cnt] = NULL;

		if (sipc)
			close(spair[0]);

		setpriority(PRIO_PROCESS, 0, job_get_integer(j->ldj, LAUNCH_JOBKEY_NICE));

		if (srl || hrl) {
			for (i = 0; i < (sizeof(limits) / sizeof(limits[0])); i++) {
				struct rlimit rl;

				if (getrlimit(limits[i].val, &rl) == -1)
					syslog(LOG_NOTICE, "getrlimit(): %m");

				if (hrl)
					rl.rlim_max = job_get_integer(hrl, limits[i].key);
				if (srl)
					rl.rlim_cur = job_get_integer(srl, limits[i].key);

				if (setrlimit(limits[i].val, &rl) == -1)
					syslog(LOG_NOTICE, "setrlimit(): %m");
			}
		}


		if (job_get_bool(j->ldj, LAUNCH_JOBKEY_INITGROUPS)) {
			const char *u = job_get_string(j->ldj, LAUNCH_JOBKEY_USERNAME);
			struct passwd *pwe;

			if (u == NULL) {
				syslog(LOG_NOTICE, "\"%s\" requires \"%s\"", LAUNCH_JOBKEY_INITGROUPS, LAUNCH_JOBKEY_USERNAME);
			} else if (launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_GID)) {
				initgroups(u, job_get_integer(j->ldj, LAUNCH_JOBKEY_GID));
			} else if ((pwe = getpwnam(u))) {
				initgroups(u, pwe->pw_gid);
			} else {
				syslog(LOG_NOTICE, "Could not find base group in order to call initgroups()");
			}
		}
		if (job_get_bool(j->ldj, LAUNCH_JOBKEY_LOWPRIORITYIO)) {
			int lowprimib[] = { CTL_KERN, KERN_PROC_LOW_PRI_IO };
			int val = 1;

			if (sysctl(lowprimib, sizeof(lowprimib) / sizeof(lowprimib[0]), NULL, NULL,  &val, sizeof(val)) == -1)
				syslog(LOG_NOTICE, "sysctl(kern.proc_low_pri_io): %m");
		}
		if (job_get_string(j->ldj, LAUNCH_JOBKEY_ROOTDIRECTORY))
			chroot(job_get_string(j->ldj, LAUNCH_JOBKEY_ROOTDIRECTORY));
		if (job_get_integer(j->ldj, LAUNCH_JOBKEY_GID) != getgid())
			setgid(job_get_integer(j->ldj, LAUNCH_JOBKEY_GID));
		if (job_get_integer(j->ldj, LAUNCH_JOBKEY_UID) != getuid())
			setuid(job_get_integer(j->ldj, LAUNCH_JOBKEY_UID));
		if (job_get_string(j->ldj, LAUNCH_JOBKEY_WORKINGDIRECTORY))
			chdir(job_get_string(j->ldj, LAUNCH_JOBKEY_WORKINGDIRECTORY));
		if (launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_UMASK))
			umask(job_get_integer(j->ldj, LAUNCH_JOBKEY_UMASK));
		if (job_get_string(j->ldj, LAUNCH_JOBKEY_STANDARDOUTPATH)) {
			int sofd = open(job_get_string(j->ldj, LAUNCH_JOBKEY_STANDARDOUTPATH), O_WRONLY|O_APPEND|O_CREAT, 0666);
			dup2(sofd, STDOUT_FILENO);
			close(sofd);
		}
		if (job_get_string(j->ldj, LAUNCH_JOBKEY_STANDARDERRORPATH)) {
			int sefd = open(job_get_string(j->ldj, LAUNCH_JOBKEY_STANDARDERRORPATH), O_WRONLY|O_APPEND|O_CREAT, 0666);
			dup2(sefd, STDERR_FILENO);
			close(sefd);
		}
		if (sipc)
			sprintf(nbuf, "%d", spair[1]);
		if (launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_ENVIRONMENTVARIABLES))
			launch_data_dict_iterate(launch_data_dict_lookup(j->ldj,
						LAUNCH_JOBKEY_ENVIRONMENTVARIABLES),
					setup_job_env, NULL);
		if (sipc)
			setenv(LAUNCHD_TRUSTED_FD_ENV, nbuf, 1);
		setsid();
		if (launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_INETDCOMPATIBILITY))
			a0 = "/usr/libexec/launchproxy";
		else
			a0 = job_get_argv0(j->ldj);
		if (!job_get_bool(j->ldj, LAUNCH_JOBKEY_ONDEMAND) && tvd.tv_sec < LAUNCHD_MIN_JOB_RUN_TIME) {
			/* Only punish short daemon life if the last exit was "bad." */
			if (j->failed_exits > 0) {
				syslog(LOG_NOTICE, "%s respawning too quickly! Sleeping %d seconds",
						job_get_argv0(j->ldj), LAUNCHD_MIN_JOB_RUN_TIME - tvd.tv_sec);
				sleep(LAUNCHD_MIN_JOB_RUN_TIME - tvd.tv_sec);
			}
		}
                if (execvp(a0, (char *const*)argv) == -1)
			syslog(LOG_ERR, "child execvp(): %m");
		exit(EXIT_FAILURE);
	}

	if (sipc) {
		close(spair[1]);
		ipc_open(_fd(spair[0]), j);
	}

	if (kevent_mod(c, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, &j->kqjob_callback) == -1) {
		syslog(LOG_WARNING, "kevent(): %m");
		return;
	} else {
		j->p = c;
		if (job_get_bool(j->ldj, LAUNCH_JOBKEY_ONDEMAND))
			job_ignore_fds(j->ldj, j->kqjob_callback);
	}
}

#ifdef PID1_REAP_ADOPTED_CHILDREN
__private_extern__ int pid1_child_exit_status = 0;
static void pid1waitpid(void)
{
	pid_t p;

	while ((p = waitpid(-1, &pid1_child_exit_status, WNOHANG)) > 0) {
		if (!launchd_check_pid(p))
			init_check_pid(p);
	}
}
#endif

static void do_shutdown(void)
{
	launchd_remove_all_jobs();
	if (getpid() == 1) {
		catatonia();
		mach_start_shutdown(SIGTERM);
	} else {
		exit(EXIT_SUCCESS);
	}
}

static void signal_callback(void *obj __attribute__((unused)), struct kevent *kev)
{
	switch (kev->ident) {
	case SIGHUP:
		update_ttys();
		reload_launchd_config();
		break;
	case SIGTERM:
		do_shutdown();
		break;
	case SIGUSR1:
		debug = !debug;
		update_lm();
		break;
	case SIGUSR2:
		verbose = !verbose;
		update_lm();
		break;
	default:
		break;
	}
}

static void update_lm(void)
{
	int oldlm, lm = LOG_UPTO(LOG_NOTICE);
	const char *lstr = "verbose";
	const char *e_vs_d = "disabled";
	if (verbose) {
		lm = LOG_UPTO(LOG_INFO);
		e_vs_d = "enabled";
	}
	if (debug) {
		lm = LOG_UPTO(LOG_DEBUG);
		lstr = "debug";
		e_vs_d = "enabled";
	}
	oldlm = setlogmask(lm);
	if (lm != oldlm)
		syslog(LOG_NOTICE, "%s logging %s", lstr, e_vs_d);
}

static void fs_callback(void *obj __attribute__((unused)), struct kevent *kev __attribute__((unused)))
{
}

static void readcfg_callback(void *obj __attribute__((unused)), struct kevent *kev __attribute__((unused)))
{
	int status;

#ifdef PID1_REAP_ADOPTED_CHILDREN
	if (getpid() == 1)
		status = pid1_child_exit_status;
	else
#endif
		waitpid(readcfg_pid, &status, 0);

	readcfg_pid = 0;

	if (WIFEXITED(status)) {
		if (WEXITSTATUS(status))
			syslog(LOG_WARNING, "Unable to read launchd.conf: launchctl exited with status: %d", WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		syslog(LOG_WARNING, "Unable to read launchd.conf: launchctl exited abnormally: %s", strsignal(WTERMSIG(status)));
	} else {
		syslog(LOG_WARNING, "Unable to read launchd.conf: launchctl exited abnormally");
	}
}

static void *mach_demand_loop(void *arg __attribute__((unused)))
{
	mach_msg_empty_rcv_t dummy;
	kern_return_t kr;
	mach_port_name_array_t members;
	mach_msg_type_number_t membersCnt;
	mach_port_status_t status;
	mach_msg_type_number_t statusCnt;
	unsigned int i;

	for (;;) {

		/*
		 * Receive indication of message on demand service
		 * ports without actually receiving the message (we'll
		 * let the actual server do that.
		 */
		kr = mach_msg(&dummy.header, MACH_RCV_MSG|MACH_RCV_LARGE,
				0, 0, mach_demand_port_set, 0, MACH_PORT_NULL);
		if (kr != MACH_RCV_TOO_LARGE) {
			syslog(LOG_WARNING, "%s(): mach_msg(): %s", __func__, mach_error_string(kr));
			continue;
		}

		/*
		 * Some port(s) now have messages on them, find out
		 * which ones (there is no indication of which port
		 * triggered in the MACH_RCV_TOO_LARGE indication).
		 */
		kr = mach_port_get_set_status(mach_task_self(),
				mach_demand_port_set, &members, &membersCnt);
		if (kr != KERN_SUCCESS) {
			syslog(LOG_WARNING, "%s(): mach_port_get_set_status(): %s", __func__, mach_error_string(kr));
			continue;
		}

		for (i = 0; i < membersCnt; i++) {
			statusCnt = MACH_PORT_RECEIVE_STATUS_COUNT;
			kr = mach_port_get_attributes(mach_task_self(), members[i],
					MACH_PORT_RECEIVE_STATUS, (mach_port_info_t)&status, &statusCnt);
			if (kr != KERN_SUCCESS) {
				syslog(LOG_WARNING, "%s(): mach_port_get_attributes(): %s", __func__, mach_error_string(kr));
				continue;
			}

			/*
			 * For each port with messages, take it out of the
			 * demand service portset, and inform the main thread
			 * that it might have to start the server responsible
			 * for it.
			 */
			if (status.mps_msgcount) {
				kr = mach_port_move_member(mach_task_self(), members[i], MACH_PORT_NULL);
				if (kr != KERN_SUCCESS) {
					syslog(LOG_WARNING, "%s(): mach_port_move_member(): %s", __func__, mach_error_string(kr));
					continue;
				}
				write(machcbwritefd, &(members[i]), sizeof(members[i]));
			}
		}

		kr = vm_deallocate(mach_task_self(), (vm_address_t) members,
				(vm_size_t) membersCnt * sizeof(mach_port_name_t));
		if (kr != KERN_SUCCESS) {
			syslog(LOG_WARNING, "%s(): vm_deallocate(): %s", __func__, mach_error_string(kr));
			continue;
		}
	}

	return NULL;
}

static void reload_launchd_config(void)
{
	struct stat sb;
	static char *ldconf = PID1LAUNCHD_CONF;
	const char *h = getenv("HOME");

	if (h && ldconf == PID1LAUNCHD_CONF)
		asprintf(&ldconf, "%s/%s", h, LAUNCHD_CONF);

	if (!ldconf)
		return;

	if (lstat(ldconf, &sb) == 0) {
		int spair[2];
		socketpair(AF_UNIX, SOCK_STREAM, 0, spair);
		readcfg_pid = fork_with_bootstrap_port(launchd_bootstrap_port);
		if (readcfg_pid == 0) {
			char nbuf[100];
			close(spair[0]);
			sprintf(nbuf, "%d", spair[1]);
			setenv(LAUNCHD_TRUSTED_FD_ENV, nbuf, 1);
			int fd = open(ldconf, O_RDONLY);
			if (fd == -1) {
				syslog(LOG_ERR, "open(\"%s\"): %m", ldconf);
				exit(EXIT_FAILURE);
			}
			dup2(fd, STDIN_FILENO);
			close(fd);
			execl(LAUNCHCTL_PATH, LAUNCHCTL_PATH, NULL);
			syslog(LOG_ERR, "execl(): %m");
			exit(EXIT_FAILURE);
		} else if (readcfg_pid == -1) {
			close(spair[0]);
			close(spair[1]);
			syslog(LOG_ERR, "fork(): %m");
			readcfg_pid = 0;
		} else {
			close(spair[1]);
			ipc_open(_fd(spair[0]), NULL);
			if (kevent_mod(readcfg_pid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, &kqreadcfg_callback) == -1)
				syslog(LOG_ERR, "kevent_mod(EVFILT_FS, &kqreadcfg_callback): %m");
		}
	}
}

static void loopback_setup(void)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	int s6 = socket(AF_INET6, SOCK_DGRAM, 0);
	struct ifaliasreq ifra;
	struct in6_aliasreq ifra6;
	struct ifreq ifr;

	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, "lo0");

	if (ioctl(s, SIOCGIFFLAGS, &ifr) == -1) {
		syslog(LOG_ERR, "ioctl(SIOCGIFFLAGS): %m");
	} else {
		ifr.ifr_flags |= IFF_UP;

		if (ioctl(s, SIOCSIFFLAGS, &ifr) == -1)
			syslog(LOG_ERR, "ioctl(SIOCSIFFLAGS): %m");
	}

	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, "lo0");

	if (ioctl(s6, SIOCGIFFLAGS, &ifr) == -1) {
		syslog(LOG_ERR, "ioctl(SIOCGIFFLAGS): %m");
	} else {
		ifr.ifr_flags |= IFF_UP;

		if (ioctl(s6, SIOCSIFFLAGS, &ifr) == -1)
			syslog(LOG_ERR, "ioctl(SIOCSIFFLAGS): %m");
	}

	memset(&ifra, 0, sizeof(ifra));
	strcpy(ifra.ifra_name, "lo0");

	((struct sockaddr_in *)&ifra.ifra_addr)->sin_family = AF_INET;
	((struct sockaddr_in *)&ifra.ifra_addr)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	((struct sockaddr_in *)&ifra.ifra_addr)->sin_len = sizeof(struct sockaddr_in);
	((struct sockaddr_in *)&ifra.ifra_mask)->sin_family = AF_INET;
	((struct sockaddr_in *)&ifra.ifra_mask)->sin_addr.s_addr = htonl(IN_CLASSA_NET);
	((struct sockaddr_in *)&ifra.ifra_mask)->sin_len = sizeof(struct sockaddr_in);

	if (ioctl(s, SIOCAIFADDR, &ifra) == -1)
		syslog(LOG_ERR, "ioctl(SIOCAIFADDR ipv4): %m");

	memset(&ifra6, 0, sizeof(ifra6));
	strcpy(ifra6.ifra_name, "lo0");

	ifra6.ifra_addr.sin6_family = AF_INET6;
	ifra6.ifra_addr.sin6_addr = in6addr_loopback;
	ifra6.ifra_addr.sin6_len = sizeof(struct sockaddr_in6);
	ifra6.ifra_prefixmask.sin6_family = AF_INET6;
	memset(&ifra6.ifra_prefixmask.sin6_addr, 0xff, sizeof(struct in6_addr));
	ifra6.ifra_prefixmask.sin6_len = sizeof(struct sockaddr_in6);
	ifra6.ifra_lifetime.ia6t_vltime = ND6_INFINITE_LIFETIME;
	ifra6.ifra_lifetime.ia6t_pltime = ND6_INFINITE_LIFETIME;

	if (ioctl(s6, SIOCAIFADDR_IN6, &ifra6) == -1)
		syslog(LOG_ERR, "ioctl(SIOCAIFADDR ipv6): %m");
 
	close(s);
	close(s6);
}

static void workaround3048875(int argc, char *argv[])
{
	int i;
	char **ap, *newargv[100], *p = argv[1];

	if (argc == 1 || argc > 2)
		return;

	newargv[0] = argv[0];
	for (ap = newargv + 1, i = 1; ap < &newargv[100]; ap++, i++) {
		if ((*ap = strsep(&p, " \t")) == NULL)
			break;
		if (**ap == '\0') {
			*ap = NULL;
			break;
		}
	}

	if (argc == i)
		return;

	execv(newargv[0], newargv);
}

static void notify_helperd(void)
{
	if (helperd && helperd->p)
		kill(helperd->p, SIGHUP);
}

static launch_data_t adjust_rlimits(launch_data_t in)
{
	static struct rlimit *l = NULL;
	static size_t lsz = sizeof(struct rlimit) * RLIM_NLIMITS;
	struct rlimit *ltmp;
	size_t i,ltmpsz;

	if (l == NULL) {
		l = malloc(lsz);
		for (i = 0; i < RLIM_NLIMITS; i++) {
			if (getrlimit(i, l + i) == -1)
				syslog(LOG_WARNING, "getrlimit(): %m");
		}
	}

	if (in) {
		ltmp = launch_data_get_opaque(in);
		ltmpsz = launch_data_get_opaque_size(in);

		if (ltmpsz > lsz) {
			syslog(LOG_WARNING, "Too much rlimit data sent!");
			ltmpsz = lsz;
		}
		
		for (i = 0; i < (ltmpsz / sizeof(struct rlimit)); i++) {
			if (ltmp[i].rlim_cur != l[i].rlim_cur || ltmp[i].rlim_max != l[i].rlim_max) {
				l[i] = ltmp[i];
				if (setrlimit(i, l + i) == -1)
					syslog(LOG_WARNING, "setrlimit(): %m");
			}
		}
	}

	return launch_data_new_opaque(l, sizeof(struct rlimit) * RLIM_NLIMITS);
}
