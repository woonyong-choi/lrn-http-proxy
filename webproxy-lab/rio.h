/*
 * rio.h - CS:APP3e 견고한 I/O(Rio) 읽기 패키지.
 *
 * 과제가 제공한 csapp.c/csapp.h 에서 프록시가 실제로 쓰는 부분만 옮겼다.
 * 원본 구현·주석은 그대로이며 저작권 표시도 유지한다(csapp.c: CS:APP3e,
 * R. Bryant, D. O'Hallaron). 나머지 래퍼(Fork/Malloc/Open_listenfd/sio 등)는
 * proxy.c 가 한 번도 호출하지 않아 제거했다.
 */
#ifndef RIO_H
#define RIO_H

#include <stddef.h>
#include <sys/types.h>

/* 견고한 I/O(Rio) 패키지의 지속 상태 */
#define RIO_BUFSIZE 8192
typedef struct {
  int rio_fd;                /* 내부 버퍼에 연결된 디스크립터 */
  int rio_cnt;               /* 내부 버퍼에서 아직 읽지 않은 바이트 수 */
  char *rio_bufptr;          /* 내부 버퍼에서 다음에 읽을 바이트 위치 */
  char rio_buf[RIO_BUFSIZE]; /* 내부 버퍼 */
} rio_t;

#define MAXLINE 8192 /* 텍스트 한 줄의 최대 길이 */
#define MAXBUF 8192  /* I/O 버퍼의 최대 크기 */

void rio_readinitb(rio_t *rp, int fd);
ssize_t rio_readnb(rio_t *rp, void *usrbuf, size_t n);
ssize_t rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen);

#endif /* RIO_H */
