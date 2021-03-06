
/*
 * providerRegister.c
 *
 * © Copyright IBM Corp. 2005, 2007
 *
 * THIS FILE IS PROVIDED UNDER THE TERMS OF THE ECLIPSE PUBLIC LICENSE
 * ("AGREEMENT"). ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS FILE
 * CONSTITUTES RECIPIENTS ACCEPTANCE OF THE AGREEMENT.
 *
 * You can obtain a current copy of the Eclipse Public License from
 * http://www.opensource.org/licenses/eclipse-1.0.php
 *
 * Author:        Adrian Schuur <schuur@de.ibm.com>
 * Based on concepts developed by Viktor Mihajlovski <mihajlov@de.ibm.com>
 *
 * Description:
 *
 * Provider registration support.
 *
 */

#include <sfcCommon/utilft.h>
#include "mlog.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <support.h>
#include <pwd.h>

#include "providerRegister.h"

static ProviderInfo forceNotFound = { "", FORCE_PROVIDER_NOTFOUND };
extern unsigned long exFlags;
extern char    *configfile;
extern int      setupControl(char *fn);
extern int      getControlChars(char *id, char **val);
extern int      getControlBool(char *id, int *val);

ProviderInfo   *qualiProvInfoPtr = NULL;
ProviderInfo   *classProvInfoPtr = NULL;
ProviderInfo   *defaultProvInfoPtr = NULL;
ProviderInfo   *interOpProvInfoPtr = NULL;
ProviderInfo   *forceNoProvInfoPtr = &forceNotFound;

static void
freeInfoPtr(ProviderInfo * info)
{
  int             n = 0;
  if (info->nextInRegister) {
    freeInfoPtr(info->nextInRegister);
  }
  free(info->className);
  free(info->providerName);
  free(info->location);
  free(info->group);
  n = 0;
  if (info->ns)
    while (info->ns[n])
      free(info->ns[n++]);
  free(info->ns);
  if (info->user)
    free(info->user);
  if (info->parms)
    free(info->parms);
  free(info);
}

static void
pRelease(ProviderRegister * br)
{
  ProviderBase   *bb = (ProviderBase *) br->hdl;
  char           *key = NULL;
  ProviderInfo   *info = NULL;
  HashTableIterator *it;

  for (it = bb->ht->ft->getFirst(bb->ht, (void **) &key, (void **) &info);
       key && it && info;
       it =
       bb->ht->ft->getNext(bb->ht, it, (void **) &key, (void **) &info)) {
    freeInfoPtr(info);
  }

  free(bb->fn);
  bb->ht->ft->release(bb->ht);
  free(br);
}

static int
addProviderToHT(ProviderInfo * info, UtilHashTable * ht)
{
  _SFCB_ENTER(TRACE_PROVIDERMGR, "addProviderToHT");
  _SFCB_TRACE(1, ("--- Add pReg entry id: %d type=%lu %s (%s)",
      info->id, info->type, info->providerName, info->className));
  ProviderInfo   *checkDummy;
  /*
   * first we add the provider to the providerRegister with the classname
   * as a key
   */

  checkDummy = ht->ft->get(ht, info->className);
  if (checkDummy) {
    /*
     * Another provider is in the register, but the newly found wants to
     * serve the same class - so we do not add it to the register but
     * append it to the one already found or to its master
     */
    if (strcmp(checkDummy->providerName, info->providerName) == 0) {
      if (checkDummy->type != info->type) {
	mlogf(M_ERROR,M_SHOW,"--- Conflicting registration types for class %s, provider %s\n", info->className, info->providerName);
	_SFCB_RETURN(1);
      }
      /* FIXME: check location, user, group, parms, unload */
      /* classname and provider name match, now check for namespace */
      int idx = 0;
      while (checkDummy->ns[idx]) {
	if (strcmp(checkDummy->ns[idx], info->ns[0]) == 0) {
	  /* double registration - discard */
	  freeInfoPtr(info);
	  _SFCB_RETURN(0);
	}
	++idx;
      }
      /* additional namespace for existing classname and provider name */
      mlogf(M_INFO,M_SHOW,"--- Collating namespaces for registration of class %s, provider %s; consider single providerRegister entry\n", info->className, info->providerName);
      checkDummy->ns=realloc(checkDummy->ns,sizeof(char*)*(idx+2));
      checkDummy->ns[idx] = strdup(info->ns[0]);
      checkDummy->ns[++idx] = NULL;
      freeInfoPtr(info);
    } else {
      /* add info to the nIR linked list */
      info->nextInRegister = checkDummy->nextInRegister;
      checkDummy->nextInRegister = info;
    }
  } else {
    ht->ft->put(ht, info->className, info);
  }
  _SFCB_RETURN(0);
}

ProviderRegister *
newProviderRegister()
{
  FILE           *in;
  char           *dir,
                 *provuser = NULL;
  char            fin[1024],
                 *stmt = NULL;
  ProviderInfo   *info = NULL;
  int             err = 0,
      n = 0,
      i;
  CntlVals        rv;
  int             id = 0;
  int             provSFCB,
                  provuid = -1;
  int             interopFound = 0;
  struct passwd  *passwd;

  ProviderRegister *br = malloc(sizeof(*br) + sizeof(ProviderBase));
  ProviderBase *bb = (ProviderBase *) (br + 1);

  setupControl(configfile);

  // Get/check provider user info
  if (getControlBool("providerDefaultUserSFCB", &provSFCB)) {
    provSFCB = 1;
  }
  if (provSFCB) {
    // This indicates that we should use the SFCB user by default
    provuid = -1;
  } else {
    if (getControlChars("providerDefaultUser", &provuser)) {
      provuid = -1;
    } else {
      errno = 0;
      passwd = getpwnam(provuser);
      if (passwd) {
        provuid = passwd->pw_uid;
      } else {
        mlogf(M_ERROR, M_SHOW,
              "--- Couldn't find username %s requested in SFCB config file. Errno: %d\n",
              provuser, errno);
        err = 1;
      }
    }
  }

  if (getControlChars("registrationDir", &dir)) {
    dir = "/var/lib/sfcb/registration";
  }

  strncpy(fin, dir, sizeof(fin)-18); /* 18 = strlen("/providerRegister")+1 */
  strcat(fin, "/providerRegister");
  in = fopen(fin, "r");
  if (in == NULL)
    mlogf(M_ERROR, M_SHOW, "--- %s not found\n", fin);

  else {

    br->hdl = bb;
    br->ft = ProviderRegisterFT;
    bb->fn = strdup(fin);
    bb->ht = UtilFactory->newHashTable(61,
                                       UtilHashTable_charKey |
                                       UtilHashTable_ignoreKeyCase);

    while (fgets(fin, sizeof(fin), in)) {
      n++;
      if (stmt)
        free(stmt);
      stmt = strdup(fin);
      switch (cntlParseStmt(fin, &rv)) {
      case 0:
        mlogf(M_ERROR, M_SHOW,
              "--- registration statement not recognized: \n\t%d: %s\n", n,
              stmt);
        err = 1;
        break;
      case 1:
        if (info) {
          if (classProvInfoPtr == NULL) {
            if (strcmp(info->className, "$ClassProvider$") == 0)
              classProvInfoPtr = info;
          } else if (defaultProvInfoPtr == NULL) {
            if (strcmp(info->className, "$DefaultProvider$") == 0)
              defaultProvInfoPtr = info;
          } else if (interOpProvInfoPtr == NULL) {
            if (strcmp(info->className, "$InterOpProvider$") == 0) {
              if (exFlags & 2)
                interOpProvInfoPtr = info;
              else
                interopFound = 1;
            }
          } else if (qualiProvInfoPtr == NULL) {
            if (strcmp(info->className, "$QualifierProvider$") == 0)
              qualiProvInfoPtr = info;
          }
          err = addProviderToHT(info, ((ProviderBase *) br->hdl)->ht);
	  if (err)  break;
        }
        info = calloc(1, sizeof(*info));
        info->className = strdup(rv.id);
        info->id = ++id;
        // Set the default provider uid
        info->uid = provuid;
        if (!provSFCB)
          info->user = provuser ? strdup(provuser) : NULL;
        break;
      case 2:
        if (strcmp(rv.id, "provider") == 0)
          info->providerName = strdup(cntlGetVal(&rv));
        else if (strcmp(rv.id, "location") == 0)
          info->location = strdup(cntlGetVal(&rv));
        else if (strcmp(rv.id, "parameters") == 0) {
          info->parms = strdup(cntlGetStr(&rv));
          for (i = strlen(info->parms); i > 0 && info->parms[i] <= ' ';
               i--) {
            info->parms[i] = 0;
          }
        } else if (strcmp(rv.id, "user") == 0) {
          info->user = strdup(cntlGetVal(&rv));
          errno = 0;
          passwd = getpwnam(info->user);
          if (passwd) {
            info->uid = passwd->pw_uid;
          } else {
            mlogf(M_ERROR, M_SHOW,
                  "--- Couldn't find username %s requested in providerRegister. Errno: %d\n",
                  info->user, errno);
            err = 1;
            break;
          }
        } else if (strcmp(rv.id, "group") == 0)
          info->group = strdup(cntlGetVal(&rv));
        else if (strcmp(rv.id, "unload") == 0) {
          char           *u;
          info->unload = 0;
          while ((u = cntlGetVal(&rv)) != NULL) {
            if (strcmp(u, "never") == 0) {
              info->unload = -1;
            } else {
              mlogf(M_ERROR, M_SHOW,
                    "--- invalid unload specification: \n\t%d: %s\n", n,
                    stmt);
              err = 1;
            }
          }
        } else if (strcmp(rv.id, "type") == 0) {
          char           *t;
          info->type = 0;
          while ((t = cntlGetVal(&rv)) != NULL) {
            if (strcmp(t, "instance") == 0)
              info->type |= INSTANCE_PROVIDER;
            else if (strcmp(t, "association") == 0)
              info->type |= ASSOCIATION_PROVIDER;
            else if (strcmp(t, "method") == 0)
              info->type |= METHOD_PROVIDER;
            else if (strcmp(t, "indication") == 0)
              info->type |= INDICATION_PROVIDER;
            else if (strcmp(t, "class") == 0)
              info->type |= CLASS_PROVIDER;
            else if (strcmp(t, "property") == 0)
              info->type |= PROPERTY_PROVIDER;
            else if (strcmp(t, "qualifier") == 0)
              info->type |= QUALIFIER_PROVIDER;
            else {
              mlogf(M_ERROR, M_SHOW,
                    "--- invalid type specification: \n\t%d: %s\n", n,
                    stmt);
              err = 1;
            }
          }
        } else if (strcmp(rv.id, "namespace") == 0) {
          int             max = 1,
              next = 0;
          char           *t;
          info->ns = malloc(sizeof(char *) * (max + 1));
          while ((t = cntlGetVal(&rv)) != NULL) {
            if (next == max) {
              max++;
              info->ns = realloc(info->ns, sizeof(char *) * (max + 1));
            }
            info->ns[next] = strdup(t);
            info->ns[++next] = NULL;
          }
        } else {
          mlogf(M_ERROR, M_SHOW,
                "--- invalid registration statement: \n\t%d: %s\n", n,
                stmt);
          err = 1;
        }
        break;
      case 3:
        break;
      }
      if (err)  break;
    }

    if (info) {
      if (err == 0) {
	addProviderToHT(info, ((ProviderBase *) br->hdl)->ht);
      }
      else {
	freeInfoPtr(info);
      }
    }
  }
  if (in) {
    fclose(in);
  }

  if (classProvInfoPtr == NULL) {
    mlogf(M_ERROR, M_SHOW,
          "--- Class provider definition not found - sfcbd will terminate\n");
    err = 1;
  }

  if (defaultProvInfoPtr == NULL)
    mlogf(M_INFO, M_SHOW,
          "--- Default provider definition not found - no instance repository available\n");

  if (qualiProvInfoPtr == NULL)
    mlogf(M_INFO, M_SHOW,
          "--- Qualifier provider definition not found - no qualifier support available\n");

  if (interOpProvInfoPtr == NULL) {
    if (exFlags & 2 && interopFound == 0)
      mlogf(M_INFO, M_SHOW,
            "--- InterOp provider definition not found - no InterOp support available\n");
    else if (interopFound)
      mlogf(M_INFO, M_SHOW,
            "--- InterOp provider definition found but not started - no InterOp support available\n");
    interOpProvInfoPtr = &forceNotFound;
  }

  if (err) {
    mlogf(M_ERROR, M_SHOW,
          "--- Broker terminated because of previous error(s)\n");
    exit(5);
  }
  if (stmt)
    free(stmt);
  return br;
}

static int
putProvider(ProviderRegister * br, const char *clsName,
            ProviderInfo * info)
{
  ProviderBase   *bb = (ProviderBase *) br->hdl;
  return bb->ht->ft->put(bb->ht, clsName, info);
}

static ProviderInfo *
getProvider(ProviderRegister * br, const char *clsName, unsigned long type)
{
  ProviderBase   *bb = (ProviderBase *) br->hdl;
  ProviderInfo   *info = bb->ht->ft->get(bb->ht, clsName);
  if (info && info->type & type) {
    return info;
  }
  return NULL;
}

static ProviderInfo *
getProviderById(ProviderRegister * br, int id)
{
  ProviderBase   *bb = (ProviderBase *) br->hdl;
  HashTableIterator *it;
  char           *key = NULL;
  ProviderInfo   *info = NULL;

  for (it = bb->ht->ft->getFirst(bb->ht, (void **) &key, (void **) &info);
       key && it && info;
       it =
       bb->ht->ft->getNext(bb->ht, it, (void **) &key, (void **) &info)) {

    while (info) {
      if (info->id == id) {
	free(it);
        return info;
      }
      info = info->nextInRegister;
    }
  }
  free(it);
  return NULL;
}

static void
resetProvider(ProviderRegister * br, int pid)
{
  ProviderBase   *bb = (ProviderBase *) br->hdl;
  HashTableIterator *it;
  char           *key = NULL;
  ProviderInfo   *info = NULL;

  for (it = bb->ht->ft->getFirst(bb->ht, (void **) &key, (void **) &info);
       key && it && info;
       it =
       bb->ht->ft->getNext(bb->ht, it, (void **) &key, (void **) &info)) {

    /*
     * reset pid, but don't return immediately; a proc may have multiple
     * provs 
     */
    while (info) {
      if (info->pid == pid) {
        info->pid = 0;
      }
      info = info->nextInRegister;
    }
  }
}

static void
removeProvider(ProviderRegister * br, const char *clsName)
{
  ProviderBase   *bb = (ProviderBase *) br->hdl;
  bb->ht->ft->remove(bb->ht, clsName);
}

static Provider_Register_FT ift = {
  1,
  pRelease,
  getProvider,
  getProviderById,
  putProvider,
  removeProvider,
  resetProvider
};

Provider_Register_FT *ProviderRegisterFT = &ift;
/* MODELINES */
/* DO NOT EDIT BELOW THIS COMMENT */
/* Modelines are added by 'make pretty' */
/* -*- Mode: C; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* vi:set ts=2 sts=2 sw=2 expandtab: */
