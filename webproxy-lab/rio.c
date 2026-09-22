/*
 * rio.c - CS:APP3e 견고한 I/O(Rio) 읽기 패키지.
 *         원본: csapp.c (CS:APP3e, R. Bryant, D. O'Hallaron).
 *         구현은 원본 그대로이고, proxy.c 가 호출하지 않는 함수만 뺐다.
 */
#include "rio.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>



/* 
 * rio_read - Unix read() 함수를 감싼 래퍼로,
 *    내부 버퍼에서 사용자 버퍼로 min(n, rio_cnt) 바이트를 옮긴다.
 *    여기서 n은 사용자가 요청한 바이트 수이고,
 *    rio_cnt는 내부 버퍼에 남아 있는 미읽기 바이트 수이다.
 *    함수 진입 시 내부 버퍼가 비어 있으면 read()를 호출해
 *    내부 버퍼를 다시 채운다.
 */
/* $begin rio_read */
static ssize_t rio_read(rio_t *rp, char *usrbuf, size_t n)
{
    int cnt;

    while (rp->rio_cnt <= 0) {  /* 버퍼가 비어 있으면 다시 채움 */
	rp->rio_cnt = read(rp->rio_fd, rp->rio_buf, 
			   sizeof(rp->rio_buf));
	if (rp->rio_cnt < 0) {
	    if (errno != EINTR) /* 시그널 핸들러에서 돌아오며 중단됨 */
		return -1;
	}
	else if (rp->rio_cnt == 0)  /* 파일 끝 */
	    return 0;
	else 
	    rp->rio_bufptr = rp->rio_buf; /* 버퍼 포인터 초기화 */
    }

    /* 내부 버퍼에서 사용자 버퍼로 min(n, rp->rio_cnt) 바이트 복사 */
    cnt = n;          
    if (rp->rio_cnt < n)   
	cnt = rp->rio_cnt;
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
void rio_readinitb(rio_t *rp, int fd) 
{
    rp->rio_fd = fd;  
    rp->rio_cnt = 0;  
    rp->rio_bufptr = rp->rio_buf;
}
/* $end rio_readinitb */

/*
 * rio_readnb - n바이트를 견고하게 읽음(버퍼 사용)
 */
/* $begin rio_readnb */
ssize_t rio_readnb(rio_t *rp, void *usrbuf, size_t n) 
{
    size_t nleft = n;
    ssize_t nread;
    char *bufp = usrbuf;
    
    while (nleft > 0) {
	if ((nread = rio_read(rp, bufp, nleft)) < 0) 
            return -1;          /* errno는 read()가 설정 */
	else if (nread == 0)
	    break;              /* 파일 끝 */
	nleft -= nread;
	bufp += nread;
    }
    return (n - nleft);         /* 0 이상 반환 */
}
/* $end rio_readnb */

/* 
 * rio_readlineb - 텍스트 한 줄을 견고하게 읽음(버퍼 사용)
 */
/* $begin rio_readlineb */
ssize_t rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen) 
{
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
		break;    /* 파일 끝, 일부 데이터는 읽음 */
	} else
	    return -1;	  /* 오류 */
    }
    *bufp = 0;
    return n-1;
}
/* $end rio_readlineb */

/**********************************
 * 견고한 I/O 루틴 래퍼
 **********************************/
