
/*
 * trace.c
 *
 * (C) Copyright IBM Corp. 2005
 *
 * THIS FILE IS PROVIDED UNDER THE TERMS OF THE ECLIPSE PUBLIC LICENSE
 * ("AGREEMENT"). ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS FILE
 * CONSTITUTES RECIPIENTS ACCEPTANCE OF THE AGREEMENT.
 *
 * You can obtain a current copy of the Eclipse Public License from
 * http://www.opensource.org/licenses/eclipse-1.0.php
 *
 * Author:        Adrian Schuur <schuur@de.ibm.com>
 * Based on concepts developed by Heidi Neumann <heidineu@de.ibm.com>
 *
 * Description:
 *
 * Trace support for sfcb.
 *
 */

#include "trace.h"
#include <errno.h>
#include "native.h"
#include <string.h>
#include <time.h>

#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdlib.h>
#include "config.h"

/*
 * ---------------------------------------------------------------------------
 */
/*
 * trace facility 
 */
/*
 * ---------------------------------------------------------------------------
 */

char           *processName = NULL;
int             providerProcess = 0;
int             idleThreadId = 0;
int             terminating = 0;

int             _sfcb_debug = 0;
unsigned long   _sfcb_trace_mask = 0;
/* use pointer indirect _sfcb_trace_mask to allow shared memory flag */
unsigned long *_ptr_sfcb_trace_mask = &_sfcb_trace_mask;
void *vpDP = NULL;
int shmid=0;
char           *_SFCB_TRACE_FILE = NULL;
int _SFCB_TRACE_TO_SYSLOG = 0;

TraceId         traceIds[] = {
  {"providerMgr", TRACE_PROVIDERMGR},
  {"providerDrv", TRACE_PROVIDERDRV},
  {"cimxmlProc", TRACE_CIMXMLPROC},
  {"httpDaemon", TRACE_HTTPDAEMON},
  {"upCalls", TRACE_UPCALLS},
  {"encCalls", TRACE_ENCCALLS},
  {"ProviderInstMgr", TRACE_PROVIDERINSTMGR},
  {"providerAssocMgr", TRACE_PROVIDERASSOCMGR},
  {"providers", TRACE_PROVIDERS},
  {"indProvider", TRACE_INDPROVIDER},
  {"internalProvider", TRACE_INTERNALPROVIDER},
  {"objectImpl", TRACE_OBJECTIMPL},
  {"xmlIn", TRACE_XMLIN},
  {"xmlOut", TRACE_XMLOUT},
  {"sockets", TRACE_SOCKETS},
  {"memoryMgr", TRACE_MEMORYMGR},
  {"msgQueue", TRACE_MSGQUEUE},
  {"xmlParsing", TRACE_XMLPARSING},
  {"responseTiming", TRACE_RESPONSETIMING},
  {"dbpdaemon", TRACE_DBPDAEMON},
  {"slp", TRACE_SLP},
  {NULL, 0}
};

/*
 * set the fg color on the console reset = 1 will restore previous color
 * set see trace.h for valid attr, fg, bg values
 * 
 * TODO: fix reset of fg color.  currently we assume it was white 
 */
void
changeTextColor(int reset)
{
  char            command[13];
  int             attr = (reset) ? RESET : currentProc % 2;
  int             fg = (reset) ? WHITE : currentProc % 7;
  int             bg = BLACK;
  if (fg == BLACK)
    fg = WHITE;
  sprintf(command, "%c[%d;%d;%dm", 0x1B, attr, fg + 30, bg + 40);
  fprintf(stderr, "%s", command);
}

void
_sfcb_trace_start(int n)
{
  if (_sfcb_debug < n) {
    _sfcb_debug = n;
  }
}

void
_sfcb_trace_stop()
{
  shmctl(shmid, IPC_RMID, 0);
  shmdt(vpDP);
  _sfcb_debug = 0;
}

void
_sfcb_trace_init()
{

  char           *var = NULL;
  char           *err = NULL;
  FILE           *ferr = NULL;
  int tryid = 0xDEB001;
  if (shmid == 0) {
    while ((shmid = shmget(tryid, sizeof(unsigned long), (IPC_EXCL | IPC_CREAT | 0660))) < 0 && (errno == EEXIST)) tryid++;
  }
  mlogf(M_INFO,M_SHOW,"--- Shared memory ID for tracing: %x\n", tryid);
  if (shmid < 0) {
    mlogf(M_ERROR,M_SHOW, "shmget(%x) failed in %s at line %d.\n", tryid, __FILE__, __LINE__ );
    abort();
  }
  else {
    vpDP = shmat( shmid, NULL, 0 );
	
    if (vpDP == (void*)-1) {// shmat returns an error
      mlogf(M_ERROR,M_SHOW, "shmat(%u,) failed with errno = %s(%u) in %s at line %d.\n", shmid, strerror(errno), errno, __FILE__, __LINE__ );
      abort();
    }
    else {
      _ptr_sfcb_trace_mask = (unsigned long *)vpDP;
    }
  }

  var = getenv("SFCB_TRACE");
  if (var != NULL) {
    _sfcb_debug = atoi(var);
  } else {
    _sfcb_debug = 0;
  }

  err = getenv("SFCB_TRACE_FILE");
  if (err != NULL) {
    if ((((ferr = fopen(err, "a")) == NULL) || fclose(ferr))) {
      mlogf(M_ERROR, M_SHOW, "--- Couldn't create trace file\n");
      return;
    }
    _SFCB_TRACE_FILE = strdup(err);
  } else {
    if (_SFCB_TRACE_FILE)
      free(_SFCB_TRACE_FILE);
    _SFCB_TRACE_FILE = NULL;
  }
}

char           *
_sfcb_format_trace(char *fmt, ...)
{
  va_list         ap;
  char           *msg = malloc(MAX_MSG_SIZE);
  va_start(ap, fmt);
  vsnprintf(msg, MAX_MSG_SIZE, fmt, ap);
  va_end(ap);
  return msg;
}

void
_sfcb_trace(int level, char *file, int line, char *msg)
{
  if (msg == NULL) return;

  struct tm       cttm;
  struct timeval  tv;
  struct timezone tz;
  long            sec = 0;
  char           *tm = NULL;
  FILE           *ferr = NULL;

  if ((_SFCB_TRACE_FILE != NULL)) {
    if ((ferr = fopen(_SFCB_TRACE_FILE, "a")) == NULL) {
      mlogf(M_ERROR, M_SHOW, "--- Couldn't open trace file");
      return;
    }
    colorTrace = 0;             /* prevents escape chars from getting into 
                                 * the file */
  } else {
    ferr = stderr;
  }

  if (gettimeofday(&tv, &tz) == 0) {
    sec = tv.tv_sec + (tz.tz_minuteswest * -1 * 60);
    tm = malloc(20 * sizeof(char));
    memset(tm, 0, 20 * sizeof(char));
    if (gmtime_r(&sec, &cttm) != NULL) {
      strftime(tm, 20, "%m/%d/%Y %H:%M:%S", &cttm);
    }

    if (*_ptr_sfcb_trace_mask) {
      if (_SFCB_TRACE_TO_SYSLOG) {
        /* ERROR is the default syslog level, if a user does not specify INFO or DEBUG. 
           ERROR guarantees output will end up in syslog */
        mlogf(M_ERROR,M_SHOW,"[%i] [%s] %d/%p --- %s(%i) : %s\n", level, tm, currentProc, (void *)pthread_self(), file,
              line, msg);
      }
      else if (colorTrace) {
        changeTextColor(0);
        fprintf(ferr, "[%i] [%s] %d/%p --- %s(%i) : %s\n", level, tm, currentProc, (void *)pthread_self(), file,
                line, msg);
        changeTextColor(1);
      }
      else {
        fprintf(ferr, "[%i] [%s] %d/%p --- %s(%i) : %s\n", level, tm, currentProc, (void *)pthread_self(), file,
                line, msg);
      }
    }

    free(tm);
    free(msg);

  }

  if ((_SFCB_TRACE_FILE != NULL)) {
    fclose(ferr);
  }
    
}

extern void _sfcb_set_trace_mask(unsigned long n)
{
  unsigned long *pulDP = (unsigned long*)vpDP;
  *pulDP = n;
}

extern void
_sfcb_set_trace_file(char *file)
{
  if (_SFCB_TRACE_FILE) {
    free(_SFCB_TRACE_FILE);
  }
  if (strcmp(file, "syslog") == 0) {
    _SFCB_TRACE_FILE = NULL;
    _SFCB_TRACE_TO_SYSLOG = 1;
  }
  else if (strcmp(file,"stderr") == 0) {
    _SFCB_TRACE_FILE = NULL;
  } else {
    _SFCB_TRACE_FILE = strdup(file);
  }
}

void
_sfcb_trap(int tn)
{
#ifdef SFCB_IX86
  char           *tp;
  int             t;
  if ((tp = getenv("SFCB_TRAP"))) {
    t = atoi(tp);
    if (tn == t)
      asm("int $3");
  }
#endif
}

sigHandler     *
setSignal(int sn, sigHandler * sh, int flags)
{
  struct sigaction newh,
                  oldh;
  newh.sa_handler = sh;
  sigemptyset(&newh.sa_mask);
  newh.sa_flags = flags;

  if (sn == SIGALRM)
    newh.sa_flags |= SA_INTERRUPT;
  else if (sn == SIGUSR2)
    newh.sa_flags |= SA_NODEFER;

  if (sigaction(sn, &newh, &oldh) < 0)
    return SIG_ERR;

  return oldh.sa_handler;
}

#ifdef UNITTEST
// Embedded unittest routine
int
trace_test()
{
  int             fail = 0;
  _sfcb_trace_start(1);
  if (_sfcb_debug < 1) {
    printf("  _sfcb_trace_start() test failed.\n");
    fail = 1;
  }
  _sfcb_trace_stop();
  if (_sfcb_debug != 0) {
    printf("  _sfcb_trace_stop() test failed.\n");
    fail = 1;
  }
  _sfcb_trace_init();
  if (_SFCB_TRACE_FILE == NULL) {
    printf("  _sfcb_trace_init() test failed.\n");
    fail = 1;
  }
  int             num = 99;
  char           *msg = _sfcb_format_trace("%s %d", "Test message", num);
  if (strcmp(msg, "Test message 99")) {
    printf("  _sfcb_format_trace() test failed, message=%s.\n", msg);
    fail = 1;
  }

  return fail;
}
#endif
/* MODELINES */
/* DO NOT EDIT BELOW THIS COMMENT */
/* Modelines are added by 'make pretty' */
/* -*- Mode: C; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* vi:set ts=2 sts=2 sw=2 expandtab: */
