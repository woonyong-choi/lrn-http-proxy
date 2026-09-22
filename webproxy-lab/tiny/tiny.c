/* $begin tinymain */
/*
 * tiny.c - GET 메서드로 정적 파일만 제공하는 단순한 반복형 HTTP/1.0 웹 서버.
 *     이 저장소에서 Tiny 의 역할은 프록시 통합 테스트의 정적 원본 서버 하나뿐이라
 *     CS:APP 원본에 있던 CGI(동적 콘텐츠) 경로는 제거했다.
 *
 * 2019/11 droh 수정
 *   - serve_static()와 clienterror()에서 sprintf() 별칭 문제 수정
 */
#include "csapp.h"

void doit(int fd);
void read_requesthdrs(rio_t* rp);
void parse_uri(char* uri, char* filename);
void serve_static(int fd, char* filename, int filesize);
void get_filetype(char* filename, char* filetype);
void clienterror(int fd, char* cause, char* errnum, char* shortmsg,
                 char* longmsg);

int main(int argc, char** argv) {
  int listenfd, connfd;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

  listenfd = Open_listenfd(argv[1]);
  while (1) {
    clientlen = sizeof(clientaddr);
    connfd = Accept(listenfd, (SA*)&clientaddr, &clientlen);
    Getnameinfo((SA*)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE,
                0);
    printf("Accepted connection from (%s, %s)\n", hostname, port);
    doit(connfd);
    Close(connfd);
  }
}

void doit(int fd) {
  struct stat sbuf;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char filename[MAXLINE];
  rio_t rio;
  ssize_t n;

  Rio_readinitb(&rio, fd);
  n = Rio_readlineb(&rio, buf, MAXLINE);
  if (n <= 0) {
    return;
  }

  printf("Request headers:\n");
  printf("%s", buf);
  if (sscanf(buf, "%s %s %s", method, uri, version) != 3) {
    clienterror(fd, buf, "400", "Bad Request",
                "Tiny could not parse the request line");
    return;
  }
  (void)version;

  if (strcasecmp(method, "GET")) {
    clienterror(fd, method, "501", "Not implemented",
                "Tiny does not implement this method");
    return;
  }
  read_requesthdrs(&rio);

  parse_uri(uri, filename);
  if (stat(filename, &sbuf) < 0) {
    clienterror(fd, filename, "404", "Not found",
                "Tiny couldn't find this file");
    return;
  }

  if (!S_ISREG(sbuf.st_mode) || !(S_IRUSR & sbuf.st_mode)) {
    clienterror(fd, filename, "403", "Forbidden",
                "Tiny couldn't read the file");
    return;
  }
  serve_static(fd, filename, (int)sbuf.st_size);
}

void read_requesthdrs(rio_t* rp) {
  char buf[MAXLINE];

  do {
    Rio_readlineb(rp, buf, MAXLINE);
    printf("%s", buf);
  } while (strcmp(buf, "\r\n"));
}

void parse_uri(char* uri, char* filename) {
  snprintf(filename, MAXLINE, ".%s", uri);
  if (uri[strlen(uri) - 1] == '/') {
    strncat(filename, "home.html", MAXLINE - strlen(filename) - 1);
  }
}

void serve_static(int fd, char* filename, int filesize) {
  int srcfd;
  char* srcp;
  char filetype[MAXLINE];
  char buf[MAXBUF];
  int len;

  get_filetype(filename, filetype);
  len = snprintf(buf, sizeof(buf),
                 "HTTP/1.0 200 OK\r\n"
                 "Server: Tiny Web Server\r\n"
                 "Cache-Control: public, max-age=2\r\n"
                 "Content-length: %d\r\n"
                 "Content-type: %s\r\n"
                 "\r\n",
                 filesize, filetype);
  Rio_writen(fd, buf, (size_t)len);

  if (filesize == 0) {
    return;
  }

  srcfd = Open(filename, O_RDONLY, 0);
  srcp = Mmap(0, (size_t)filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
  Close(srcfd);
  Rio_writen(fd, srcp, (size_t)filesize);
  Munmap(srcp, (size_t)filesize);
}

void get_filetype(char* filename, char* filetype) {
  if (strstr(filename, ".html")) {
    strcpy(filetype, "text/html");
  } else if (strstr(filename, ".gif")) {
    strcpy(filetype, "image/gif");
  } else if (strstr(filename, ".png")) {
    strcpy(filetype, "image/png");
  } else {
    strcpy(filetype, "text/plain");
  }
}

void clienterror(int fd, char* cause, char* errnum, char* shortmsg,
                 char* longmsg) {
  char buf[MAXLINE], body[MAXLINE];
  int body_len, header_len;

  body_len = snprintf(body, sizeof(body),
                      "<html><title>Tiny Error</title>"
                      "<body bgcolor=\"ffffff\">\r\n"
                      "%s: %s\r\n"
                      "<p>%s: %s\r\n"
                      "<hr><em>The Tiny Web server</em>\r\n",
                      errnum, shortmsg, longmsg, cause);

  header_len = snprintf(buf, sizeof(buf),
                        "HTTP/1.0 %s %s\r\n"
                        "Content-type: text/html\r\n"
                        "Content-length: %d\r\n"
                        "\r\n",
                        errnum, shortmsg, body_len);
  Rio_writen(fd, buf, (size_t)header_len);
  Rio_writen(fd, body, (size_t)body_len);
}
