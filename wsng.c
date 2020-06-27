#include    <dirent.h>
#include    <stdio.h>
#include    <stdlib.h>
#include    <strings.h>
#include    <string.h>
#include    <netdb.h>
#include    <errno.h>
#include    <signal.h>
#include    <sys/param.h>
#include    <sys/stat.h>
#include    <sys/time.h>
#include    <sys/types.h>
#include    <sys/wait.h>
#include    <time.h>
#include    <unistd.h>
#include    "socklib.h"
#include    "wsng_util.h"

/*
 * ws.c - a web server
 *
 *    usage: ws [ -c configfilenmame ]
 * features: supports the GET command only
 *           runs in the current directory
 *           forks a new child to handle each request
 *           needs many additional features
 *
 *  compile: cc ws.c socklib.c -o ws
 *  history: 2012-04-23 removed extern declaration for fdopen (it's in stdio.h)
 *  history: 2012-04-21 more minor cleanups, expanded some fcn comments
 *  history: 2010-04-24 cleaned code, merged some of MK's ideas
 *  history: 2008-05-01 removed extra fclose that was causing double free
 */


#define PORTNUM 80
#define SERVER_ROOT "."
#define CONFIG_FILE "wsng.conf"
#define VERSION     "1"
#define MAX_RQ_LEN  4096
#define LINELEN     1024
#define PARAM_LEN   128
#define VALUE_LEN   512
#define MAXVARS     2

char myhost[MAXHOSTNAMELEN];
int myport;
char* full_hostname();


typedef struct content_type {
    char* ext;
    char* content;
    struct content_type* next;
} content_type;

content_type* head = NULL;

#define oops(m,x) {perror(m); exit(x);}

/*
 * prototypes
 */

int     startup(int, char* a[], char[], int*);
void    read_til_crnl(FILE *);
void    process_rq(char*, FILE*);
void    bad_request(FILE*);
void    cannot_do(FILE* fp);
void    do_404(char* item, FILE* fp);
void    do_500(char* item, FILE* fp);
void    do_cat(char* f, FILE* fpsock);
void    do_exec(char* prog, FILE* fp);
void    do_ls(char* dir, FILE* fp);
int     ends_in_cgi(char* f);
int     ends_in_html(char* f);

char*   file_type(char* f);
void    header(FILE* fp, int code, char* msg, char* content_type);
int     isadir(char* f);
char*   modify_argument(char* arg, int len);
int     not_exist(char* f);
int     no_access(char* f);
void    fatal(char*, char*);
void    handle_call(int);
int     read_request(FILE*, char*, int);
char*   readline(char*, int, FILE*);
void    free_table(content_type*);
char*   check_if_index(char* dir);
char*   query_string(char* f);



int main(int ac, char* av[])
{
    int sock, fd;

    sock = startup(ac, av, myhost, &myport);

    printf("wsng%s started.  host=%s port=%d\n", VERSION, myhost, myport);

    while (1) {
        fd = accept(sock, NULL, NULL); /* take a call  */
        if (fd == -1)
            perror("accept");
        else
            handle_call(fd);           /* handle call  */
    }
    free_table(head);
    return 0;
}


/*
 * handle_call(fd) - serve the request arriving on fd
 * summary: fork, then get request, then process request
 *    rets: child exits with 1 for error, 0 for ok
 *    note: closes fd in parent
 */
void handle_call(int fd)
{
    int pid = fork();
    FILE *fpin, *fpout;
    char request[MAX_RQ_LEN];

    if (pid == -1) {
        perror("fork");
        return;
    }
    /* child: buffer socket and talk with client */
    if (pid == 0) {
        fpin = fdopen(fd, "r");
        fpout = fdopen(fd, "w");
        if (fpin == NULL || fpout == NULL)
            exit(1);

        if (read_request(fpin, request, MAX_RQ_LEN) == -1)
            exit(1);
        printf("got a call: request = %s", request);

        process_rq(request, fpout);
        fflush(fpout);      /* send data to client   */
        exit(0);            /* child is done         */
    }
    /* parent: close fd and return to take next call */
    waitpid(pid, NULL, WNOHANG);
    close(fd);
}


/*
 * read the http request into rq not to exceed rqlen
 * return -1 for error, 0 for success
 */
int read_request(FILE *fp, char rq[], int rqlen)
{
    /* null means EOF or error. Either way there is no request */
    if (readline(rq, rqlen, fp) == NULL)
        return -1;
    read_til_crnl(fp);
    return 0;
}


void read_til_crnl(FILE *fp)
{
    char buf[MAX_RQ_LEN];

    while (readline(buf, MAX_RQ_LEN, fp) != NULL && strcmp(buf, "\r\n") != 0) {}
}


/*
 * readline -- read in a line from fp, stop at \n
 *    args: buf - place to store line
 *          len - size of buffer
 *          fp  - input stream
 *    rets: NULL at EOF else the buffer
 *    note: will not overflow buffer, but will read until \n or EOF
 *          thus will lose data if line exceeds len-2 chars
 *    note: like fgets but will always read until \n even if it loses data
 */
char *readline(char *buf, int len, FILE *fp)
{
    int space = len - 2;
    char *cp = buf;
    int c;

    while ((c = getc(fp)) != '\n' && c != EOF) {
        if (space-- > 0)
            *cp++ = c;
    }
    if (c == '\n')
        *cp++ = c;
    *cp = '\0';
    return (c == EOF && cp == buf ? NULL : buf);
}


/*
 * initialization function
 *  1. process command line args
 *      handles -c configfile
 *  2. open config file
 *      read rootdir, port
 *  3. chdir to rootdir
 *  4. open a socket on port
 *  5. gets the hostname
 *  6. return the socket
 *       later, it might set up logfiles, check config files,
 *         arrange to handle signals
 *
 *  returns: socket as the return value
 *       the host by writing it into host[]
 *       the port by writing it into *portnump
 */
int startup(int ac, char *av[], char host[], int *portnump)
{
    int sock;
    int portnum = PORTNUM;
    char* configfile = CONFIG_FILE;
    int pos;
    void process_config_file(char *, int *);

    for (pos = 1; pos < ac; pos++) {
        if (strcmp(av[pos], "-c") == 0) {
            if (++pos < ac)
                configfile = av[pos];
            else
                fatal("missing arg for -c", NULL);
        }
    }
    process_config_file(configfile, &portnum);

    sock = make_server_socket(portnum);
    if (sock == -1)
        oops("making socket", 2);
    strcpy(myhost, full_hostname());
    *portnump = portnum;
    return sock;
}

/* ------------------------------------------------------ *
   Data structure to manage different file types defined in
   wsng.conf.
   ------------------------------------------------------ */

content_type* init_type(content_type* head, char* ext, char* content) {
    content_type *newtype = (content_type*)malloc(sizeof(content_type));
    if (newtype != NULL) {
        newtype->ext = ext;
        newtype->content = content;
        newtype->next = NULL;
    }

    return newtype;
}


content_type* push_type(content_type* table, char* ext, char* content) {
    content_type* curr;
    curr = head;
    while (curr->next != NULL)
        curr = curr->next;

    content_type* newtype = init_type(NULL, ext, content);
    curr->next = newtype;
    return head;
}


void free_table(content_type* head) {
    content_type* curr, *tmp;
    curr = head;
    while (curr->next != NULL) {
        tmp = curr;
        curr = curr->next;
        free(tmp);
    }
}


/*
 * opens file or dies
 * reads file for lines with the format
 *   port ###
 *   server_root path
 * at the end, return the portnum by loading *portnump
 * and chdir to the rootdir
 */
void process_config_file(char *conf_file, int *portnump)
{
    FILE *fp;
    char rootdir[VALUE_LEN] = SERVER_ROOT;
    char param[PARAM_LEN];
    char val1[VALUE_LEN];
    char val2[VALUE_LEN];
    int port;
    int read_param(FILE *, char *, int, char *, int, char*);

    /* open the file */
    if ((fp = fopen(conf_file, "r")) == NULL)
        fatal("Cannot open config file %s", conf_file);

    content_type* table;
    table = init_type(head, "DEFAULT", "text/plain");
    head = table;

    /* extract the settings */
    while (read_param(fp, param, PARAM_LEN, val1, VALUE_LEN, val2) != EOF) {
        if (strcasecmp(param, "server_root") == 0)
            strcpy(rootdir, val1);

        if (strcasecmp(param, "port") == 0)
            port = atoi(val1);

        if (strcasecmp(param, "type") == 0)
            table = push_type(table, val1, val2);
    }
    content_type* ptr;
    ptr = head;
    while (ptr->next != NULL) {
        ptr = ptr->next;
    }

    fclose(fp);
    /* act on the settings */
    if (chdir(rootdir) == -1)
        oops("cannot change to rootdir", 2);
    *portnump = port;
    return;
}


/*
 * read_param:
 *   purpose -- read next parameter setting line from fp
 *   details -- a param-setting line looks like  name value
 *      for example:  port 4444
 *     extra -- skip over lines that start with # and those
 *      that do not contain two strings
 *   returns -- EOF at eof and 1 on good data
 *
 */
int read_param(FILE *fp, char *name, int nlen, char* val1, int vlen, char* val2)
{
    char line[LINELEN];
    int c;
    char fmt[100];

    snprintf(fmt, sizeof(int)*4+1, "%%%ds%%%ds%%%ds", nlen, vlen, vlen);
    /* read in next line and if the line is too long, read until \n */
    while (fgets(line, LINELEN, fp) != NULL) {
        if (line[strlen(line)-1] != '\n')
            while ((c = getc(fp)) != '\n' && c != EOF) {}

        int nval = sscanf(line, fmt, name, val1, val2);
        if ((nval == 2 || nval == 3) && *name != '#')
            return 1;
    }
    return EOF;
}



/* ------------------------------------------------------ *
   process_rq( char *rq, FILE *fpout)
   do what the request asks for and write reply to fp
   rq is HTTP command:  GET /foo/bar.html HTTP/1.0
   ------------------------------------------------------ */
void process_rq(char *rq, FILE *fp)
{
    char    cmd[MAX_RQ_LEN], arg[MAX_RQ_LEN];
    char    *item, *modify_argument();

    if (sscanf(rq, "%s%s", cmd, arg) != 2) {
        bad_request(fp);
        return;
    }

    item = query_string(modify_argument(arg, MAX_RQ_LEN));
    if (strcmp(cmd, "HEAD") == 0)
        header(fp, 200, "OK", "text/plain");
    else if (strcmp(cmd, "GET") != 0)
        cannot_do(fp);
    else if (not_exist(item))
        do_404(item, fp);
    else if (no_access(item) == -1)
        do_500(item, fp);
    else if (isadir(item))
        do_ls(item, fp);
    else if (ends_in_cgi(item))
        do_exec(item, fp);
    else
        do_cat(item, fp);
}


/*
 * modify_argument
 *  purpose: many roles
 *      security - remove all ".." components in paths
 *      cleaning - if arg is "/" convert to "."
 *  returns: pointer to modified string
 *     args: array containing arg and length of that array
 */
char* modify_argument(char *arg, int len)
{
    char    *nexttoken;
    char    *copy = malloc(len);

    if (copy == NULL)
        oops("memory error", 1);
    /* remove all ".." components from path */
    /* by tokeninzing on "/" and rebuilding */
    /* the string without the ".." items    */

    *copy = '\0';

    nexttoken = strtok(arg, "/");
    while (nexttoken != NULL) {
        if (strcmp(nexttoken, "..") != 0) {
            if (*copy)
                strcat(copy, "/");
            strcat(copy, nexttoken);
        }
        nexttoken = strtok(NULL, "/");
    }
    strcpy(arg, copy);
    free(copy);

    /* the array is now cleaned up */
    /* handle a special case       */

    if (strcmp(arg, "") == 0)
        strcpy(arg, ".");
    return arg;
}

/* ------------------------------------------------------ *
   the reply header thing: all functions need one
   if content_type is NULL then don't send content type
   ------------------------------------------------------ */

/*
 * show_time - displays time in a format fit for human consumption.
 * @rets - null terminated string.
 */
char* show_time()
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) == -1)
        perror("gettimeofday");

    return ctime(&tv.tv_sec);
}


void header(FILE *fp, int code, char *msg, char *content_type)
{
    fprintf(fp, "HTTP/1.0 %d %s ", code, msg);
    fprintf(fp, "Date: %s ", show_time());
    fprintf(fp, "Server: %s ", full_hostname());
    if (content_type)
        fprintf(fp, "Content-type: %s\r\n", content_type);
}

/* ------------------------------------------------------ *
   simple functions first:
    bad_request(fp)     bad request syntax
        cannot_do(fp)       unimplemented HTTP command
    and do_404(item,fp)     no such object
   ------------------------------------------------------ */

void bad_request(FILE *fp)
{
    header(fp, 400, "Bad Request", "text/plain");
    fprintf(fp, "\r\nI cannot understand your request\r\n");
}

void cannot_do(FILE *fp)
{
    header(fp, 501, "Not Implemented", "text/plain");
    fprintf(fp, "\r\n");
    fprintf(fp, "That command is not yet implemented\r\n");
}

void do_404(char *item, FILE *fp)
{
    header(fp, 404, "Not Found", "text/plain");
    fprintf(fp, "\r\n");
    fprintf(fp, "The item you requested: %s\r\nis not found\r\n", item);
}
void do_500(char *item, FILE *fp)
{
    header(fp, 500, "Internal Server Error", "text/plain");
    fprintf(fp, "\r\n");
    fprintf(fp, "%s\r\n: no permission\r\n", item);
}


/* ------------------------------------------------------ *
   the directory listing section
   isadir() uses stat, not_exist() uses stat
   do_ls runs ls. It should not
   ------------------------------------------------------ */

int isadir(char *f)
{
    struct stat info;

    return (stat(f, &info) != -1 && S_ISDIR(info.st_mode));
}


int not_exist(char *f)
{
    struct stat info;

    return(stat(f, &info) == -1 && errno == ENOENT);
}


int no_access(char *f)
{
    return access(f, R_OK);
}


char* check_if_index(char* dir)
{
    DIR *tmp_dir;
    struct dirent *file;
    char buf[1024];

    tmp_dir = opendir(dir);
    while ((file = readdir(tmp_dir)) != NULL) {
        strcpy(buf, file->d_name);
        char* ptr;
        if ((ptr = strrchr(buf, '.')))
            *(ptr) = '\0';

        if ((ends_in_html(file->d_name) == 1) && (strcmp(buf, "index") == 0))
            return file->d_name;

        if ((ends_in_cgi(file->d_name) == 1) && (strcmp(buf, "index") == 0))
            return file->d_name;
    }
    return "";
}


/*
 * lists the directory named by 'dir'
 * sends the listing to the stream at fp
 */
void do_ls(char *dir, FILE *fp)
{
    header(fp, 200, "OK", "text/plain");
    fprintf(fp, "\r\n");
    fflush(fp);

    DIR *tmp_dir;
    struct dirent *file;
    struct stat info_p;
    char  modestr[11];
    char buf[1024];
    char* index = check_if_index(dir);

    if (strcmp(index, "") != 0) {
        snprintf(buf, sizeof(char*)*3, "%s/%s", dir, index);
        if (strcmp(index, "index.html") == 0)
            do_cat(buf, fp);

        if (strcmp(index, "index.cgi") == 0)
            do_exec(buf, fp);
    } else {
        tmp_dir = opendir(dir);
        fprintf(fp, "<html>\n");
        while ((file = readdir(tmp_dir)) != NULL) {
            snprintf(buf, sizeof(char*)*3, "%s/%s", dir, file->d_name);
            stat(buf, &info_p);
            mode_to_letters(info_p.st_mode, modestr);
            fprintf(fp, "%s"    , modestr);
            fprintf(fp, "%4d "  , (int) info_p.st_nlink);
            fprintf(fp, "%-8s " , uid_to_name(info_p.st_uid));
            fprintf(fp, "%-8s " , gid_to_name(info_p.st_gid));
            fprintf(fp, "%5ld " , (int64_t)info_p.st_size);
            fprintf(fp, "%s "   , fmt_time(info_p.st_mtime, DATE_FMT));
            fprintf(fp, "<a href=\"%s\">%s</a><br></br>\n",
                    buf, file->d_name);
        }
        fprintf(fp, "</html>\n");
        closedir(tmp_dir);
    }
}

/* ------------------------------------------------------ *
   the cgi stuff.  function to check extension and
   one to run the program.
   ------------------------------------------------------ */
char* file_type(char *f)
/* returns 'extension' of file */
{
    char *cp;
    if ((cp = strrchr(f, '.' )) != NULL)
        return cp+1;
    return "";
}

char* query_string(char *f)
{
    char* ptr;

    if ((ptr = strrchr(f, '?')) != NULL) {
        *(ptr) = '\0';
        setenv("QUERY_STRING", ptr+1, 1);
        setenv("REQUEST_METHOD", "GET", 1);
    }
    return f;
}

int ends_in_cgi(char *f)
{
    return (strcmp(file_type(f), "cgi") == 0);
}

int ends_in_html(char *f)
{
    return (strcmp(file_type(f), "html") == 0);
}

void do_exec(char *prog, FILE *fp)
{
    int fd = fileno(fp);

    header(fp, 200, "OK", NULL);
    fflush(fp);

    dup2(fd, 1);
    dup2(fd, 2);
    execl(prog, prog, NULL);
    perror(prog);
}
/* ------------------------------------------------------ *
   do_cat(filename,fp)
   sends back contents after a header
   ------------------------------------------------------ */

void do_cat(char *f, FILE *fpsock)
{
    char *extension = file_type(f);
    char *content = "text/plain";
    FILE *fpfile;
    int c;

    content_type* typeptr;
    typeptr = head;
    while (typeptr->next != NULL) {
        printf("%s %s\n", typeptr->ext, typeptr->content);
        if (strcmp(typeptr->ext, extension) == 0)
            content = typeptr->content;

        typeptr = typeptr->next;
    }

    fpfile = fopen(f, "r");
    if (fpfile != NULL) {
        header(fpsock, 200, "OK", content);
        fprintf(fpsock, "\r\n");
        while ((c = getc(fpfile)) != EOF)
            putc(c, fpsock);
        fclose(fpfile);
    }
}

char * full_hostname()
/*
 * returns full `official' hostname for current machine
 * NOTE: this returns a ptr to a static buffer that is
 *       overwritten with each call. ( you know what to do.)
 */
{
    struct hostent *hp;
    char hname[MAXHOSTNAMELEN];
    static char fullname[MAXHOSTNAMELEN];

    if (gethostname(hname, MAXHOSTNAMELEN) == -1) {
        perror("gethostname");
        exit(1);
    }
    hp = gethostbyname(hname);        /* get info about host  */
    if (hp == NULL)                   /*   or die     */
        return NULL;
    strcpy(fullname, hp->h_name);     /* store foo.bar.com    */
    return fullname;                  /* and return it    */
}


void fatal(char *fmt, char *str)
{
    fprintf(stderr, fmt, str);
    exit(1);
}
