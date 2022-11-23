/* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the 
 *     GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh 
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 *
 * Updated 11/2022
 *   - Support the HTTP POST and HEAD method.
 *   - Add SIGCCHLD handler reap CGI children.
 *   - Using malloc,rio_readn,and rio_writen instead of mmap and rio_writen.
 *   - Using Pthreads.
 *
 * Update 11/2022
 *   - Using pre_threads instead of Pthreads.
*/

#include "csapp.h"
#include "sbuf.h"


#ifdef DEBUG
#define DBG_PRINTF(...) fprintf(stderr,__VA_ARGS__)
#else
#define DBG_PRINTF(...)
#endif    /* DEBUG */

#define SBUFSIZE 4
#define THREAD_INIT_NUM 1
#define THREAD_MAX_NUM 1024


void doit(int fd);
void read_requesthdrs(rio_t *rp, char *method, char *cgiargs);
int parse_uri(char *uri, char *filename, char *cgiargs, char *method);
void serve_static(int fd, char *filename, int filesize,char *method);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs,char *method);
void clienterror(int fd, char *cause, char *errnum, 
		 char *shortmsg, char *longmsg);
void sigchld_handler(int sig);
void *serve_thread(void *vargp);
void create_thread(int first, int last);
void *adjust_thread(void *vargp);
void init(void);

static sbuf_t sbuf;
static int numthread;

typedef struct infothread{
   pthread_t tid;
   sem_t mutex;
}infotd;
static infotd thread[THREAD_MAX_NUM];

int main(int argc, char **argv) 
{
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    /* Check command line args */
    if (argc != 2) {
     	fprintf(stderr, "usage: %s <port>\n", argv[0]);
	    exit(1);
     } 
    if (Signal(SIGCHLD, sigchld_handler) == SIG_ERR){
         unix_error("sigchld handler error\n");
    }
    if (Signal(SIGPIPE, SIG_IGN) == SIG_ERR) { /* Terminated or stopped child */
		unix_error("signal pipe error! \n");
 	}

    listenfd = Open_listenfd(argv[1]);
    
    numthread = THREAD_INIT_NUM;
    sbuf_init(&sbuf, SBUFSIZE);    
    create_thread(0, numthread);
    Pthread_create(&tid, NULL, adjust_thread, NULL); 
    while (1) { 
    	clientlen = sizeof(struct sockaddr_storage);
	    connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen); //line:netp:tiny:accept
        Getnameinfo((SA *) &clientaddr, clientlen, hostname, MAXLINE, 
                    port, MAXLINE, 0);
        //printf("Accepted connection from (%s, %s)\n", hostname, port);
        sbuf_insert(&sbuf, connfd);
    }
    return 0;
}
/* $end tinymain */

/* begin create_thread */
void create_thread(int first, int last )
{ 
  int i;
   for (i = first; i < last; i++) {
     Sem_init(&(thread[i].mutex), 0, 1);
     
     int *fd = (int*)Malloc(sizeof(int));
     *fd = i;
     Pthread_create(&(thread[i].tid), NULL, serve_thread, fd);
     DBG_PRINTF("create thread[%d]\n",i);
  } 
} 
/*end create_thread */

void *adjust_thread(void *vargp) 
{ 
  sbuf_t *sp = &sbuf;

  while (1) {
     if (sbuf_full(sp)) {
        if (numthread == THREAD_MAX_NUM) {
			fprintf(stderr, "too many threads , can`t adjust.\n");
        continue;
		}
     int double_numtd = 2 * numthread;
     create_thread(numthread, double_numtd);
     DBG_PRINTF("add double thread!\n");
     numthread = double_numtd;
     continue;
     }
     if (sbuf_empty(sp)) {
       if (numthread == 1)
         continue;
       int half_numtd = numthread / 2;
       int i;
       for (i = half_numtd; i < numthread; i++) {
         DBG_PRINTF("cancel thread[%d]\n",i);
         P(&(thread[i].mutex));
         Pthread_cancel(thread[i].tid);
         V(&(thread[i].mutex));
       }
       numthread = half_numtd;
       continue;
     }
  }
}  
/* Thread routine */
void *serve_thread(void *vargp)
{
    int fd = *(int*)vargp;
    Pthread_detach(pthread_self());
    Free(vargp);
    while (1) {
    P(&(thread[fd].mutex));
    int connfd = sbuf_remove(&sbuf);
    doit(connfd);
    Close(connfd);
    V(&(thread[fd].mutex));
    }
} 

/*
 * doit - handle one HTTP request/response transaction
 */
/* $begin doit */
void doit(int fd) 
{
    int is_static;
    struct stat sbuf;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char filename[MAXLINE], cgiargs[MAXLINE];
    rio_t rio;

    /* Read request line and headers */
    Rio_readinitb(&rio, fd);
    if (!Rio_readlineb(&rio, buf, MAXLINE))  //line:netp:doit:readrequest
        return;
    //printf("%s", buf);
    sscanf(buf, "%s %s %s", method, uri, version);       //line:netp:doit:parserequest
    if (!(strcasecmp(method, "GET") == 0  || strcasecmp(method, "HEAD") == 0 || 
         strcasecmp(method, "POST") == 0))  {                     //line:netp:doit:beginrequesterr
        clienterror(fd, method, "501", "Not Implemented",
                    "Tiny does not implement this method");
        return;
    }                                                    //line:netp:doit:endrequesterr
    read_requesthdrs(&rio, method, cgiargs);                              //line:netp:doit:readrequesthdrs
      
    /* Parse URI from HTTP request */
    is_static = parse_uri(uri, filename, cgiargs, method);       //line:netp:doit:staticcheck
    if (stat(filename, &sbuf) < 0) {                     //line:netp:doit:beginnotfound
	clienterror(fd, filename, "404", "Not found",
		    "Tiny couldn't find this file");
	return;
    }                                                     //line:netp:doit:endnotfound

    if  (is_static) { /* Serve static content */          
	if ( !(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) { //line:netp:doit:readable
	    clienterror(fd, filename, "403", "Forbidden",
			"Tiny couldn't read the file");
	    return;
	}
	serve_static(fd, filename, sbuf.st_size, method);        //line:netp:doit:servestatic
    }
    else  { /* Serve dynamic content */
	if (!( S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) { //line:netp:doit:executable
	    clienterror(fd, filename, "403", "Forbidden",
			"Tiny couldn't run the CGI program");
	    return;
	}
	    serve_dynamic(fd, filename, cgiargs, method);            //line:netp:doit:servedynamic
	}
}
/* $end doit */

/*
 * read_requesthdrs - read HTTP request headers
 */
/* $begin read_requesthdrs */
void  read_requesthdrs(rio_t *rp, char *method, char *cgiargs) 
{
    char buf[MAXLINE], cgi[MAXLINE];
    int len = -1;
    do {
      Rio_readlineb(rp, buf, MAXLINE);
	  //printf("%s", buf);
	  
      if (strcasecmp(method, "POST") == 0 && strncasecmp(buf, "Content-Length:", 15) == 0) {
		sscanf(buf,"Content-Length: %d",&len);
        }
      } while(strcmp (buf, "\r\n")) ;          //line:netp:readhdrs:checkterm
	  if (strcasecmp(method, "POST") == 0 && len != -1) {
        Rio_readnb(rp, cgi, len);
        strcpy(cgiargs, cgi);
	  }
}
/* $end read_requesthdrs */

/*
 * parse_uri - parse URI into filename and CGI args
 *             return 0 if dynamic content, 1 if static
 */
/* $begin parse_uri */
int parse_uri(char *uri, char *filename, char *cgiargs, char *method) 
{
    char *ptr;

    if (!strstr(uri, "cgi-bin")) {  /* Static content */ //line:netp:parseuri:isstatic
	strcpy(cgiargs, "");                             //line:netp:parseuri:clearcgi
	strcpy(filename, ".");                           //line:netp:parseuri:beginconvert1
	strcat(filename, uri);                           //line:netp:parseuri:endconvert1
	if (uri[strlen(uri)-1] == '/')                   //line:netp:parseuri:slashcheck
	    strcat(filename, "home.html");               //line:netp:parseuri:appenddefault
	return 1;
    } 
    else {  /* Dynamic content */                        //line:netp:parseuri:isdynamic
       if ( strcasecmp(method, "GET") == 0 || strcasecmp(method, "HEAD") == 0 ) {
		ptr = index(uri, '?');                           //line:netp:parseuri:beginextract
	 	if  (ptr) {
		  strcpy(cgiargs, ptr+1);
		  *ptr = '\0';
		}
	    else { 
	      strcpy(cgiargs, "");                         //line:netp:parseuri:endextract
        }
	   }
    strcpy(filename, ".");                           //line:netp:parseuri:beginconvert2
	strcat(filename, uri);                           //line:netp:parseuri:endconvert2
	return 0;
    } 
}
/* $end parse_uri */

/*
 * serve_static - copy a file back to the client 
 */
/* $begin serve_static */
void serve_static(int fd, char *filename, int filesize, char* method)
{
    int srcfd;
    char *srcp, filetype[MAXLINE], buf[MAXBUF];

    /* Send response headers to client */
    get_filetype(filename, filetype);    //line:netp:servestatic:getfiletype
    sprintf(buf, "HTTP/1.0 200 OK\r\n"); //line:netp:servestatic:beginserve
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n", filesize);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: %s\r\n\r\n", filetype);
    Rio_writen(fd, buf, strlen(buf));    //line:netp:servestatic:endserve

    /* Send response body to client */
    
    /*HEAD method , doesn`t need to send body to client*/
    if (strcasecmp(method, "HEAD") == 0) 
		return;
    
    srcfd = Open(filename, O_RDONLY, 0); //line:netp:servestatic:open 

    //srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0); //line:netp:servestatic:mmap
    srcp = (char*)Malloc(filesize);  // line:netp::servestatic:malloc
    Rio_readn(srcfd,srcp,filesize); 

    Close(srcfd);                       //line:netp:servestatic:close
    Rio_writen(fd, srcp, filesize);     //line:netp:servestatic:write
    //Munmap(srcp, filesize);             //line:netp:servestatic:munmap
    Free(srcp);    //line:netp:servestatic:free

}

/*
 * get_filetype - derive file type from file name
 */
void get_filetype(char *filename, char *filetype) 
{
    if (strstr(filename, ".html"))
	strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif"))
	strcpy(filetype, "image/gif");
    else if (strstr(filename, ".png"))
	strcpy(filetype, "image/png");
    else if (strstr(filename, ".jpg"))
	strcpy(filetype, "image/jpeg");
	else if (strstr(filename, ".mpeg"))
    strcpy(filetype,"video/mpeg");
    else if (strstr(filename, ".flv"))
    strcpy(filetype,"video/flv");
    else if (strstr(filename, ".mp4"))
    strcpy(filetype,"video/mp4");
    else
	strcpy(filetype, "text/plain");
}  
/* $end serve_static */

/*
 * serve_dynamic - run a CGI program on behalf of the client
 */
/* $begin serve_dynamic */
void serve_dynamic(int fd, char *filename, char *cgiargs, char* method) 
{
    char buf[MAXLINE], *emptylist[] = { NULL };

    /* Return first part of HTTP response */
    sprintf(buf, "HTTP/1.0 200 OK\r\n"); 
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));
    
    if (Fork() == 0) { /* Child */ //line:netp:servedynamic:fork
      if (Signal(SIGPIPE, SIG_DFL) == SIG_ERR) { /* Terminated or stopped child */
        unix_error("signal pipe error! \n");
      }
    
	/* Real server would set all CGI vars here */
    setenv("QUERY_STRING", cgiargs, 1); //line:netp:servedynamic:setenv
    setenv("REQUEST_METHOD", method, 1);
	Dup2(fd, STDOUT_FILENO);         /* Redirect stdout to client */ //line:netp:servedynamic:dup2
	Execve(filename, emptylist, environ); /* Run CGI program */ //line:netp:servedynamic:execve
    } 
}
/* $end serve_dynamic */

/*
 * clienterror - returns an error message to the client
 */
/* $begin clienterror */
void clienterror(int fd, char *cause, char *errnum, 
		 char *shortmsg, char *longmsg) 
{
    char buf[MAXLINE];

    /* Print the HTTP response headers */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n\r\n");
    Rio_writen(fd, buf, strlen(buf));

    /* Print the HTTP response body */
    sprintf(buf, "<html><title>Tiny Error</title>");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<body bgcolor=""ffffff"">\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "%s: %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<p>%s: %s\r\n", longmsg, cause);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<hr><em>The Tiny Web server</em>\r\n");
    Rio_writen(fd, buf, strlen(buf));
}
/* $end clienterror */

/*
* sigchld_handler - recycle cgi child process
*
*/
/* begin sigchld_handler */
void sigchld_handler(int sig)
{
    int olderrno = errno;
    int status;    
    pid_t pid;
	while ((pid = waitpid(-1,&status,WNOHANG)) > 0) {
       printf("recycle child process! \n");
    }
	
   errno = olderrno;
}
/* $end signal_handler */

