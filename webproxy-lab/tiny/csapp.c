/*
 * csapp.c - CS:APP3e 책용 함수 모음
 *
 * 2016/10 reb 수정:
 *   - 음수를 처리하지 못하던 sio_ltoa 버그 수정
 *
 * 2016/2 droh 수정:
 *   - open_clientfd와 open_listenfd가 더 자연스럽게 실패하도록 개선
 *
 * 2014/8 droh 수정:
 *   - open_clientfd와 open_listenfd의 새 버전을 재진입 가능하고
 *     프로토콜 독립적으로 개선
 *
 *   - 프로토콜 독립적인 inet_ntop, inet_pton 함수 추가
 *     inet_ntoa와 inet_aton 함수는 더 이상 권장되지 않음
 *
 * 2014/7 droh 수정:
 *   - 재진입 가능한 sio(시그널 안전 I/O) 루틴 추가
 *
 * 2013/4 droh 수정:
 *   - rio_readlineb의 경계 조건 버그 수정
 *   - rio_readnb의 중복된 EINTR 검사 제거
 */
/* $begin csapp.c */
#include "csapp.h"

/**************************
 * 오류 처리 함수
 **************************/
/* $begin errorfuns */
/* $begin unixerror */
void unix_error(char* msg) /* Unix 방식 오류 */
{
  fprintf(stderr, "%s: %s\n", msg, strerror(errno));
  exit(0);
}
/* $end unixerror */

void posix_error(int code, char* msg) /* Posix 방식 오류 */
{
  fprintf(stderr, "%s: %s\n", msg, strerror(code));
  exit(0);
}

void csapp_gai_error(int code, char* msg) /* getaddrinfo 방식 오류 */
{
  fprintf(stderr, "%s: %s\n", msg, gai_strerror(code));
  exit(0);
}

void app_error(char* msg) /* 애플리케이션 오류 */
{
  fprintf(stderr, "%s\n", msg);
  exit(0);
}
/* $end errorfuns */

void dns_error(char* msg) /* 더 이상 권장되지 않는 gethostbyname 오류 */
{
  fprintf(stderr, "%s\n", msg);
  exit(0);
}

/*********************************************
 * Unix 프로세스 제어 함수 래퍼
 ********************************************/

/* $begin forkwrapper */
pid_t Fork(void) {
  pid_t pid;

  if ((pid = fork()) < 0) unix_error("Fork error");
  return pid;
}
/* $end forkwrapper */

void Execve(const char* filename, char* const argv[], char* const envp[]) {
  if (execve(filename, argv, envp) < 0) unix_error("Execve error");
}

/* $begin wait */
pid_t Wait(int* status) {
  pid_t pid;

  if ((pid = wait(status)) < 0) unix_error("Wait error");
  return pid;
}
/* $end wait */

pid_t Waitpid(pid_t pid, int* iptr, int options) {
  pid_t retpid;

  if ((retpid = waitpid(pid, iptr, options)) < 0) unix_error("Waitpid error");
  return (retpid);
}

/* $begin kill */
void Kill(pid_t pid, int signum) {
  int rc;

  if ((rc = kill(pid, signum)) < 0) unix_error("Kill error");
}
/* $end kill */

void Pause() {
  (void)pause();
  return;
}

unsigned int Sleep(unsigned int secs) {
  unsigned int rc;

  if ((rc = sleep(secs)) < 0) unix_error("Sleep error");
  return rc;
}

unsigned int Alarm(unsigned int seconds) { return alarm(seconds); }

void Setpgid(pid_t pid, pid_t pgid) {
  int rc;

  if ((rc = setpgid(pid, pgid)) < 0) unix_error("Setpgid error");
  return;
}

pid_t Getpgrp(void) { return getpgrp(); }

/************************************
 * Unix 시그널 함수 래퍼
 ***********************************/

/* $begin sigaction */
handler_t* Signal(int signum, handler_t* handler) {
  struct sigaction action, old_action;

  action.sa_handler = handler;
  sigemptyset(&action.sa_mask); /* 현재 처리 중인 종류의 시그널 차단 */
  action.sa_flags = SA_RESTART; /* 가능하면 시스템 호출을 다시 시작 */

  if (sigaction(signum, &action, &old_action) < 0) {
    unix_error("Signal error");
  }
  return (old_action.sa_handler);
}
/* $end sigaction */

void Sigprocmask(int how, const sigset_t* set, sigset_t* oldset) {
  if (sigprocmask(how, set, oldset) < 0) unix_error("Sigprocmask error");
  return;
}

void Sigemptyset(sigset_t* set) {
  if (sigemptyset(set) < 0) unix_error("Sigemptyset error");
  return;
}

void Sigfillset(sigset_t* set) {
  if (sigfillset(set) < 0) unix_error("Sigfillset error");
  return;
}

void Sigaddset(sigset_t* set, int signum) {
  if (sigaddset(set, signum) < 0) unix_error("Sigaddset error");
  return;
}

void Sigdelset(sigset_t* set, int signum) {
  if (sigdelset(set, signum) < 0) unix_error("Sigdelset error");
  return;
}

int Sigismember(const sigset_t* set, int signum) {
  int rc;
  if ((rc = sigismember(set, signum)) < 0) unix_error("Sigismember error");
  return rc;
}

int Sigsuspend(const sigset_t* set) {
  int rc = sigsuspend(set); /* always returns -1 */
  if (errno != EINTR) unix_error("Sigsuspend error");
  return rc;
}

/*************************************************************
 * Sio(시그널 안전 I/O) 패키지
 * 시그널 핸들러 안에서도 안전하게 사용할 수 있는 단순한 재진입형
 * 출력 함수 모음
 *************************************************************/

/* 내부 sio 함수 */

/* $begin sioprivate */
/* sio_reverse - 문자열을 뒤집음(K&R 참고) */
static void sio_reverse(char s[]) {
  int c, i, j;

  for (i = 0, j = strlen(s) - 1; i < j; i++, j--) {
    c = s[i];
    s[i] = s[j];
    s[j] = c;
  }
}

/* sio_ltoa - long 값을 b진수 문자열로 변환(K&R 참고) */
static void sio_ltoa(long v, char s[], int b) {
  int c, i = 0;
  int neg = v < 0;

  if (neg) v = -v;

  do {
    s[i++] = ((c = (v % b)) < 10) ? c + '0' : c - 10 + 'a';
  } while ((v /= b) > 0);

  if (neg) s[i++] = '-';

  s[i] = '\0';
  sio_reverse(s);
}

/* sio_strlen - 문자열 길이를 반환(K&R 참고) */
static size_t sio_strlen(char s[]) {
  int i = 0;

  while (s[i] != '\0') ++i;
  return i;
}
/* $end sioprivate */

/* 공개 Sio 함수 */
/* $begin siopublic */

ssize_t sio_puts(char s[]) /* 문자열 출력 */
{
  return write(STDOUT_FILENO, s, sio_strlen(s));  // line:csapp:siostrlen
}

ssize_t sio_putl(long v) /* long 값 출력 */
{
  char s[128];

  sio_ltoa(v, s, 10); /* K&R의 itoa() 아이디어 기반 */  // line:csapp:sioltoa
  return sio_puts(s);
}

void sio_error(char s[]) /* 오류 메시지를 출력하고 종료 */
{
  sio_puts(s);
  _exit(1);  // line:csapp:sioexit
}
/* $end siopublic */

/*******************************
 * SIO 루틴 래퍼
 ******************************/
ssize_t Sio_putl(long v) {
  ssize_t n;

  if ((n = sio_putl(v)) < 0) sio_error("Sio_putl error");
  return n;
}

ssize_t Sio_puts(char s[]) {
  ssize_t n;

  if ((n = sio_puts(s)) < 0) sio_error("Sio_puts error");
  return n;
}

void Sio_error(char s[]) { sio_error(s); }

/********************************
 * Unix I/O 루틴 래퍼
 ********************************/

int Open(const char* pathname, int flags, mode_t mode) {
  int rc;

  if ((rc = open(pathname, flags, mode)) < 0) unix_error("Open error");
  return rc;
}

ssize_t Read(int fd, void* buf, size_t count) {
  ssize_t rc;

  if ((rc = read(fd, buf, count)) < 0) unix_error("Read error");
  return rc;
}

ssize_t Write(int fd, const void* buf, size_t count) {
  ssize_t rc;

  if ((rc = write(fd, buf, count)) < 0) unix_error("Write error");
  return rc;
}

off_t Lseek(int fildes, off_t offset, int whence) {
  off_t rc;

  if ((rc = lseek(fildes, offset, whence)) < 0) unix_error("Lseek error");
  return rc;
}

void Close(int fd) {
  int rc;

  if ((rc = close(fd)) < 0) unix_error("Close error");
}

int Select(int n, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
           struct timeval* timeout) {
  int rc;

  if ((rc = select(n, readfds, writefds, exceptfds, timeout)) < 0)
    unix_error("Select error");
  return rc;
}

int Dup2(int fd1, int fd2) {
  int rc;

  if ((rc = dup2(fd1, fd2)) < 0) unix_error("Dup2 error");
  return rc;
}

void Stat(const char* filename, struct stat* buf) {
  if (stat(filename, buf) < 0) unix_error("Stat error");
}

void Fstat(int fd, struct stat* buf) {
  if (fstat(fd, buf) < 0) unix_error("Fstat error");
}

/*********************************
 * 디렉터리 함수 래퍼
 *********************************/

DIR* Opendir(const char* name) {
  DIR* dirp = opendir(name);

  if (!dirp) unix_error("opendir error");
  return dirp;
}

struct dirent* Readdir(DIR* dirp) {
  struct dirent* dep;

  errno = 0;
  dep = readdir(dirp);
  if ((dep == NULL) && (errno != 0)) unix_error("readdir error");
  return dep;
}

int Closedir(DIR* dirp) {
  int rc;

  if ((rc = closedir(dirp)) < 0) unix_error("closedir error");
  return rc;
}

/***************************************
 * 메모리 매핑 함수 래퍼
 ***************************************/
void* Mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset) {
  void* ptr;

  if ((ptr = mmap(addr, len, prot, flags, fd, offset)) == ((void*)-1))
    unix_error("mmap error");
  return (ptr);
}

void Munmap(void* start, size_t length) {
  if (munmap(start, length) < 0) unix_error("munmap error");
}

/***************************************************
 * 동적 메모리 할당 함수 래퍼
 ***************************************************/

void* Malloc(size_t size) {
  void* p;

  if ((p = malloc(size)) == NULL) unix_error("Malloc error");
  return p;
}

void* Realloc(void* ptr, size_t size) {
  void* p;

  if ((p = realloc(ptr, size)) == NULL) unix_error("Realloc error");
  return p;
}

void* Calloc(size_t nmemb, size_t size) {
  void* p;

  if ((p = calloc(nmemb, size)) == NULL) unix_error("Calloc error");
  return p;
}

void Free(void* ptr) { free(ptr); }

/******************************************
 * 표준 I/O 함수 래퍼
 ******************************************/
void Fclose(FILE* fp) {
  if (fclose(fp) != 0) unix_error("Fclose error");
}

FILE* Fdopen(int fd, const char* type) {
  FILE* fp;

  if ((fp = fdopen(fd, type)) == NULL) unix_error("Fdopen error");

  return fp;
}

char* Fgets(char* ptr, int n, FILE* stream) {
  char* rptr;

  if (((rptr = fgets(ptr, n, stream)) == NULL) && ferror(stream))
    app_error("Fgets error");

  return rptr;
}

FILE* Fopen(const char* filename, const char* mode) {
  FILE* fp;

  if ((fp = fopen(filename, mode)) == NULL) unix_error("Fopen error");

  return fp;
}

void Fputs(const char* ptr, FILE* stream) {
  if (fputs(ptr, stream) == EOF) unix_error("Fputs error");
}

size_t Fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
  size_t n;

  if (((n = fread(ptr, size, nmemb, stream)) < nmemb) && ferror(stream))
    unix_error("Fread error");
  return n;
}

void Fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
  if (fwrite(ptr, size, nmemb, stream) < nmemb) unix_error("Fwrite error");
}

/****************************
 * 소켓 인터페이스 래퍼
 ****************************/

int Socket(int domain, int type, int protocol) {
  int rc;

  if ((rc = socket(domain, type, protocol)) < 0) unix_error("Socket error");
  return rc;
}

void Setsockopt(int s, int level, int optname, const void* optval, int optlen) {
  int rc;

  if ((rc = setsockopt(s, level, optname, optval, optlen)) < 0)
    unix_error("Setsockopt error");
}

void Bind(int sockfd, struct sockaddr* my_addr, int addrlen) {
  int rc;

  if ((rc = bind(sockfd, my_addr, addrlen)) < 0) unix_error("Bind error");
}

void Listen(int s, int backlog) {
  int rc;

  if ((rc = listen(s, backlog)) < 0) unix_error("Listen error");
}

int Accept(int s, struct sockaddr* addr, socklen_t* addrlen) {
  int rc;

  if ((rc = accept(s, addr, addrlen)) < 0) unix_error("Accept error");
  return rc;
}

void Connect(int sockfd, struct sockaddr* serv_addr, int addrlen) {
  int rc;

  if ((rc = connect(sockfd, serv_addr, addrlen)) < 0)
    unix_error("Connect error");
}

/*******************************
 * 프로토콜 독립 래퍼
 *******************************/
/* $begin getaddrinfo */
void Getaddrinfo(const char* node, const char* service,
                 const struct addrinfo* hints, struct addrinfo** res) {
  int rc;

  if ((rc = getaddrinfo(node, service, hints, res)) != 0)
    csapp_gai_error(rc, "Getaddrinfo error");
}
/* $end getaddrinfo */

void Getnameinfo(const struct sockaddr* sa, socklen_t salen, char* host,
                 size_t hostlen, char* serv, size_t servlen, int flags) {
  int rc;

  if ((rc = getnameinfo(sa, salen, host, hostlen, serv, servlen, flags)) != 0)
    csapp_gai_error(rc, "Getnameinfo error");
}

void Freeaddrinfo(struct addrinfo* res) { freeaddrinfo(res); }

void Inet_ntop(int af, const void* src, char* dst, socklen_t size) {
  if (!inet_ntop(af, src, dst, size)) unix_error("Inet_ntop error");
}

void Inet_pton(int af, const char* src, void* dst) {
  int rc;

  rc = inet_pton(af, src, dst);
  if (rc == 0)
    app_error("inet_pton error: invalid dotted-decimal address");
  else if (rc < 0)
    unix_error("Inet_pton error");
}

/*******************************************
 * DNS 인터페이스 래퍼
 *
 * 참고: 이 함수들은 스레드 안전하지 않으므로 더 이상 권장되지 않습니다.
 * 대신 getaddrinfo와 getnameinfo를 사용하세요.
 ***********************************/

/* $begin gethostbyname */
struct hostent* Gethostbyname(const char* name) {
  struct hostent* p;

  if ((p = gethostbyname(name)) == NULL) dns_error("Gethostbyname error");
  return p;
}
/* $end gethostbyname */

struct hostent* Gethostbyaddr(const char* addr, int len, int type) {
  struct hostent* p;

  if ((p = gethostbyaddr(addr, len, type)) == NULL)
    dns_error("Gethostbyaddr error");
  return p;
}

/************************************************
 * Pthreads 스레드 제어 함수 래퍼
 ************************************************/

void Pthread_create(pthread_t* tidp, pthread_attr_t* attrp,
                    void* (*routine)(void*), void* argp) {
  int rc;

  if ((rc = pthread_create(tidp, attrp, routine, argp)) != 0)
    posix_error(rc, "Pthread_create error");
}

void Pthread_cancel(pthread_t tid) {
  int rc;

  if ((rc = pthread_cancel(tid)) != 0) posix_error(rc, "Pthread_cancel error");
}

void Pthread_join(pthread_t tid, void** thread_return) {
  int rc;

  if ((rc = pthread_join(tid, thread_return)) != 0)
    posix_error(rc, "Pthread_join error");
}

/* $begin detach */
void Pthread_detach(pthread_t tid) {
  int rc;

  if ((rc = pthread_detach(tid)) != 0) posix_error(rc, "Pthread_detach error");
}
/* $end detach */

void Pthread_exit(void* retval) { pthread_exit(retval); }

pthread_t Pthread_self(void) { return pthread_self(); }

void Pthread_once(pthread_once_t* once_control, void (*init_function)()) {
  pthread_once(once_control, init_function);
}

/*******************************
 * Posix 세마포어 래퍼
 *******************************/

void Sem_init(sem_t* sem, int pshared, unsigned int value) {
  if (sem_init(sem, pshared, value) < 0) unix_error("Sem_init error");
}

void P(sem_t* sem) {
  if (sem_wait(sem) < 0) unix_error("P error");
}

void V(sem_t* sem) {
  if (sem_post(sem) < 0) unix_error("V error");
}

/****************************************
 * Rio 패키지 - 견고한 I/O 함수
 ****************************************/

/*
 * rio_readn - n바이트를 견고하게 읽음(버퍼 없음)
 */
/* $begin rio_readn */
ssize_t rio_readn(int fd, void* usrbuf, size_t n) {
  size_t nleft = n;
  ssize_t nread;
  char* bufp = usrbuf;

  while (nleft > 0) {
    if ((nread = read(fd, bufp, nleft)) < 0) {
      if (errno == EINTR) /* 시그널 핸들러에서 돌아오며 중단됨 */
        nread = 0;        /* read()를 다시 호출 */
      else
        return -1; /* errno는 read()가 설정 */
    } else if (nread == 0)
      break; /* 파일 끝 */
    nleft -= nread;
    bufp += nread;
  }
  return (n - nleft); /* 0 이상 반환 */
}
/* $end rio_readn */

/*
 * rio_writen - n바이트를 견고하게 씀(버퍼 없음)
 */
/* $begin rio_writen */
ssize_t rio_writen(int fd, void* usrbuf, size_t n) {
  size_t nleft = n;
  ssize_t nwritten;
  char* bufp = usrbuf;

  while (nleft > 0) {
    if ((nwritten = write(fd, bufp, nleft)) <= 0) {
      if (errno == EINTR) /* 시그널 핸들러에서 돌아오며 중단됨 */
        nwritten = 0;     /* write()를 다시 호출 */
      else
        return -1; /* errno는 write()가 설정 */
    }
    nleft -= nwritten;
    bufp += nwritten;
  }
  return n;
}
/* $end rio_writen */

/*
 * rio_read - Unix read() 함수를 감싼 래퍼로,
 *    내부 버퍼에서 사용자 버퍼로 min(n, rio_cnt) 바이트를 옮긴다.
 *    여기서 n은 사용자가 요청한 바이트 수이고,
 *    rio_cnt는 내부 버퍼에 남아 있는 미읽기 바이트 수이다.
 *    함수 진입 시 내부 버퍼가 비어 있으면 read()를 호출해
 *    내부 버퍼를 다시 채운다.
 */
/* $begin rio_read */
static ssize_t rio_read(rio_t* rp, char* usrbuf, size_t n) {
  int cnt;

  while (rp->rio_cnt <= 0) { /* 버퍼가 비어 있으면 다시 채움 */
    rp->rio_cnt = read(rp->rio_fd, rp->rio_buf, sizeof(rp->rio_buf));
    if (rp->rio_cnt < 0) {
      if (errno != EINTR) /* 시그널 핸들러에서 돌아오며 중단됨 */
        return -1;
    } else if (rp->rio_cnt == 0) /* 파일 끝 */
      return 0;
    else
      rp->rio_bufptr = rp->rio_buf; /* 버퍼 포인터 초기화 */
  }

  /* 내부 버퍼에서 사용자 버퍼로 min(n, rp->rio_cnt) 바이트 복사 */
  cnt = n;
  if (rp->rio_cnt < n) cnt = rp->rio_cnt;
  memcpy(usrbuf, rp->rio_bufptr, cnt);
  rp->rio_bufptr += cnt;
  rp->rio_cnt -= cnt;
  return cnt;
}
/* $end rio_read */

/*
 * rio_readinitb - 디스크립터를 읽기 버퍼와 연결하고 버퍼를 초기화
 */
/* $begin rio_readinitb */
void rio_readinitb(rio_t* rp, int fd) {
  rp->rio_fd = fd;
  rp->rio_cnt = 0;
  rp->rio_bufptr = rp->rio_buf;
}
/* $end rio_readinitb */

/*
 * rio_readnb - n바이트를 견고하게 읽음(버퍼 사용)
 */
/* $begin rio_readnb */
ssize_t rio_readnb(rio_t* rp, void* usrbuf, size_t n) {
  size_t nleft = n;
  ssize_t nread;
  char* bufp = usrbuf;

  while (nleft > 0) {
    if ((nread = rio_read(rp, bufp, nleft)) < 0)
      return -1; /* errno는 read()가 설정 */
    else if (nread == 0)
      break; /* 파일 끝 */
    nleft -= nread;
    bufp += nread;
  }
  return (n - nleft); /* 0 이상 반환 */
}
/* $end rio_readnb */

/*
 * rio_readlineb - 텍스트 한 줄을 견고하게 읽음(버퍼 사용)
 */
/* $begin rio_readlineb */
ssize_t rio_readlineb(rio_t* rp, void* usrbuf, size_t maxlen) {
  int n, rc;
  char c, *bufp = usrbuf;

  for (n = 1; n < maxlen; n++) {
    if ((rc = rio_read(rp, &c, 1)) == 1) {
      *bufp++ = c;
      if (c == '\n') {
        n++;
        break;
      }
    } else if (rc == 0) {
      if (n == 1)
        return 0; /* 파일 끝, 읽은 데이터 없음 */
      else
        break; /* 파일 끝, 일부 데이터는 읽음 */
    } else
      return -1; /* 오류 */
  }
  *bufp = 0;
  return n - 1;
}
/* $end rio_readlineb */

/**********************************
 * 견고한 I/O 루틴 래퍼
 **********************************/
ssize_t Rio_readn(int fd, void* ptr, size_t nbytes) {
  ssize_t n;

  if ((n = rio_readn(fd, ptr, nbytes)) < 0) unix_error("Rio_readn error");
  return n;
}

void Rio_writen(int fd, void* usrbuf, size_t n) {
  if (rio_writen(fd, usrbuf, n) != n) unix_error("Rio_writen error");
}

void Rio_readinitb(rio_t* rp, int fd) { rio_readinitb(rp, fd); }

ssize_t Rio_readnb(rio_t* rp, void* usrbuf, size_t n) {
  ssize_t rc;

  if ((rc = rio_readnb(rp, usrbuf, n)) < 0) unix_error("Rio_readnb error");
  return rc;
}

ssize_t Rio_readlineb(rio_t* rp, void* usrbuf, size_t maxlen) {
  ssize_t rc;

  if ((rc = rio_readlineb(rp, usrbuf, maxlen)) < 0)
    unix_error("Rio_readlineb error");
  return rc;
}

/********************************
 * 클라이언트/서버 도우미 함수
 ********************************/
/*
 * open_clientfd - <hostname, port> 서버에 연결을 열고
 *     읽기/쓰기가 가능한 소켓 디스크립터를 반환한다.
 *     이 함수는 재진입 가능하며 프로토콜에 독립적이다.
 *
 *     오류 시 반환값:
 *       -2: getaddrinfo 오류
 *       -1: 그 외 오류(errno 설정됨)
 */
/* $begin open_clientfd */
int open_clientfd(char* hostname, char* port) {
  int clientfd, rc;
  struct addrinfo hints, *listp, *p;

  /* 가능한 서버 주소 목록 가져오기 */
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_socktype = SOCK_STREAM; /* 연결 열기 */
  hints.ai_flags = AI_NUMERICSERV; /* 숫자 포트 인자 사용 */
  hints.ai_flags |= AI_ADDRCONFIG; /* 연결 시 권장 설정 */
  if ((rc = getaddrinfo(hostname, port, &hints, &listp)) != 0) {
    fprintf(stderr, "getaddrinfo failed (%s:%s): %s\n", hostname, port,
            gai_strerror(rc));
    return -2;
  }

  /* 성공적으로 연결할 수 있는 주소를 목록에서 찾기 */
  for (p = listp; p; p = p->ai_next) {
    /* 소켓 디스크립터 생성 */
    if ((clientfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
      continue; /* Socket failed, try the next */

    /* 서버에 연결 */
    if (connect(clientfd, p->ai_addr, p->ai_addrlen) != -1) break; /* Success */
    if (close(clientfd) < 0) {
      /* Connect failed, try another */  // line:netp:openclientfd:closefd
      fprintf(stderr, "open_clientfd: close failed: %s\n", strerror(errno));
      return -1;
    }
  }

  /* 정리 */
  freeaddrinfo(listp);
  if (!p) /* 모든 연결 시도 실패 */
    return -1;
  else /* 마지막 연결 시도 성공 */
    return clientfd;
}
/* $end open_clientfd */

/*
 * open_listenfd - 포트에서 대기(listening) 소켓을 열고 반환한다.
 *     이 함수는 재진입 가능하며 프로토콜에 독립적이다.
 *
 *     오류 시 반환값:
 *       -2: getaddrinfo 오류
 *       -1: 그 외 오류(errno 설정됨)
 */
/* $begin open_listenfd */
int open_listenfd(char* port) {
  struct addrinfo hints, *listp, *p;
  int listenfd, rc, optval = 1;

  /* 가능한 서버 주소 목록 가져오기 */
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_socktype = SOCK_STREAM;             /* 연결 요청 수락 */
  hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG; /* 모든 IP 주소에서 대기 */
  hints.ai_flags |= AI_NUMERICSERV;            /* 숫자 포트 번호 사용 */
  if ((rc = getaddrinfo(NULL, port, &hints, &listp)) != 0) {
    fprintf(stderr, "getaddrinfo failed (port %s): %s\n", port,
            gai_strerror(rc));
    return -2;
  }

  /* bind 가능한 주소를 목록에서 찾기 */
  for (p = listp; p; p = p->ai_next) {
    /* 소켓 디스크립터 생성 */
    if ((listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
      continue; /* Socket failed, try the next */

    /* bind 시 "Address already in use" 오류를 줄임 */
    setsockopt(listenfd, SOL_SOCKET,
               SO_REUSEADDR,  // line:netp:csapp:setsockopt
               (const void*)&optval, sizeof(int));

    /* 디스크립터를 주소에 bind */
    if (bind(listenfd, p->ai_addr, p->ai_addrlen) == 0) break; /* Success */
    if (close(listenfd) < 0) { /* Bind failed, try the next */
      fprintf(stderr, "open_listenfd close failed: %s\n", strerror(errno));
      return -1;
    }
  }

  /* 정리 */
  freeaddrinfo(listp);
  if (!p) /* 동작하는 주소가 없음 */
    return -1;

  /* 연결 요청을 받을 준비가 된 listening 소켓으로 설정 */
  if (listen(listenfd, LISTENQ) < 0) {
    close(listenfd);
    return -1;
  }
  return listenfd;
}
/* $end open_listenfd */

/****************************************************
 * 재진입 가능한 프로토콜 독립 도우미 함수 래퍼
 ****************************************************/
int Open_clientfd(char* hostname, char* port) {
  int rc;

  if ((rc = open_clientfd(hostname, port)) < 0)
    unix_error("Open_clientfd error");
  return rc;
}

int Open_listenfd(char* port) {
  int rc;

  if ((rc = open_listenfd(port)) < 0) unix_error("Open_listenfd error");
  return rc;
}

/* $end csapp.c */
