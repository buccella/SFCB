/*
 * $Id: instance.c,v 1.45 2008/11/21 20:23:51 mchasal Exp $
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
 * Author:        Frank Scheffler
 * Contributions: Adrian Schuur <schuur@de.ibm.com>
 *
 * Description:
 *
 * CMPIInstance implementation.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sfcCommon/utilft.h>
#include "native.h"
#include "instance.h"

#include "objectImpl.h"
#include "providerMgr.h"
#include "config.h"

#ifdef SFCB_IX86
#define SFCB_ASM(x) asm(x)
#else
#define SFCB_ASM(x)
#endif

extern int      ClInstanceGetPropertyAt(ClInstance * inst, int id,
                                        CMPIData *data, char **name,
                                        unsigned long *quals);
extern int      ClObjectLocateProperty(ClObjectHdr * hdr, ClSection * prps,
                                       const char *id);
extern int      ClInstanceGetPropertyCount(ClInstance * inst);
extern int      ClInstanceAddProperty(ClInstance * inst, const char *id,
                                      CMPIData d);
extern ClInstance *ClInstanceNew(const char *ns, const char *cn);
extern void     ClInstanceFree(ClInstance * inst);
extern const char *ClInstanceGetClassName(ClInstance * inst);
extern const char *ClInstanceGetNameSpace(ClInstance * inst);
extern unsigned long ClSizeInstance(ClInstance * inst);
extern ClInstance *ClInstanceRebuild(ClInstance * inst, void *area);
extern void     ClInstanceRelocateInstance(ClInstance * inst);
extern CMPIArray *native_make_CMPIArray(CMPIData *av, CMPIStatus *rc,
                                        ClObjectHdr * hdr);
extern CMPIObjectPath *getObjectPath(char *path, char **msg);
CMPIObjectPath *internal_new_CMPIObjectPath(int mode,
                                            const char *nameSpace,
                                            const char *className,
                                            CMPIStatus *rc);
extern const char *ClObjectGetClString(ClObjectHdr * hdr, ClString * id);
extern int      sfcb_comp_CMPIValue(CMPIValue * val1, CMPIValue * val2,
                                    CMPIType type);
extern long     addClString(ClObjectHdr * hdr, const char *str);

extern CMPIBroker *Broker;

struct native_instance {
  CMPIInstance    instance;
  int             refCount;
  int             mem_state;
  int             filtered;
  char          **property_list; /* the filter property list (not list of all props) */
  char          **key_list;
};

static pthread_mutex_t gopMtx = PTHREAD_MUTEX_INITIALIZER;

#ifdef HAVE_DEFAULT_PROPERTIES
static pthread_mutex_t ifdpMtx = PTHREAD_MUTEX_INITIALIZER;
static int     instFillDefaultProperties(struct native_instance *inst,
                                          const char *ns, const char *cn);
#endif

static CMPIStatus __ift_internal_setPropertyFilter(CMPIInstance * instance,
                                                   const char **propertyList,
                                                   const char **keys);

/****************************************************************************/

static void
__release_list(char **list)
{
  if (list) {
    char          **tmp = list;
    while (*tmp)
      free(*tmp++);
    free(list);
  }
}

static char   **
__duplicate_list(const char **list)
{
  char          **result = NULL;

  if (list) {
    size_t          size = 1;
    char          **tmp = (char **) list;

    while (*tmp++)
      ++size;
    result = calloc(1, size * sizeof(char *));
    for (tmp = result; *list; tmp++) {
      *tmp = strdup(*list++);
    }
  }
  return result;
}

static char **__make_key_list(const CMPIObjectPath * cop)
{
  char **keys = NULL;
  int i, j;

  if (cop) {
    j = CMGetKeyCount(cop, NULL);

    if (j) {
      /* allocate extra element since list needs to be NULL terminated */
      keys = calloc(j + 1, sizeof(char *));
      for (i = 0; i < j; i++) {
        CMPIString *keyName;
        CMGetKeyAt(cop, i , &keyName, NULL);
        keys[i] = strdup(CMGetCharsPtr(keyName, NULL));
      }
    }
  }

  return keys;
}

void
memLinkInstance(CMPIInstance *ci)
{
  struct native_instance *i = (struct native_instance *) ci;
  memLinkEncObj(i, &i->mem_state);
}

void
memUnlinkInstance(CMPIInstance *ci)
{
  struct native_instance *i = (struct native_instance *) ci;
  memUnlinkEncObj(i->mem_state);
}

static int
__contained_list(char **list, const char *name)
{
  if (list) {
    while (*list) {
      if (strcasecmp(*list++, name) == 0)
        return 1;
    }
  }
  return 0;
}

/****************************************************************************/

static CMPIStatus
__ift_release(CMPIInstance *instance)
{
  struct native_instance *i = (struct native_instance *) instance;

  if (!instance->hdl) {
    CMReturn(CMPI_RC_ERR_INVALID_HANDLE);
  }

  if (i->mem_state && i->mem_state != MEM_RELEASED) {
    __release_list(i->property_list);
    __release_list(i->key_list);
    ClInstanceFree((ClInstance *) instance->hdl);
    memUnlinkEncObj(i->mem_state);
    i->mem_state = MEM_RELEASED;
    free(i);
    CMReturn(CMPI_RC_OK);
  }
  CMReturn(CMPI_RC_ERR_FAILED);
}

static CMPIInstance *
__ift_clone(const CMPIInstance *instance, CMPIStatus *rc)
{
  struct native_instance *i = (struct native_instance *) instance;
  struct native_instance *new;

  if (!instance->hdl) {
    if (rc)
      CMSetStatus(rc, CMPI_RC_ERR_INVALID_HANDLE);
    return NULL;
  }

  new = malloc(sizeof(*new));

  new->refCount = 0;
  new->mem_state = MEM_NOT_TRACKED;
  new->property_list = __duplicate_list((const char **) i->property_list);
  new->key_list = __duplicate_list((const char **) i->key_list);
  new->filtered = i->filtered;

  ((CMPIInstance *) new)->hdl =
      ClInstanceRebuild((ClInstance *) instance->hdl, NULL);
  ((CMPIInstance *) new)->ft = instance->ft;

  if (rc)
      CMSetStatus(rc, CMPI_RC_OK);
  return (CMPIInstance *) new;
}

CMPIData
__ift_internal_getPropertyAt(const CMPIInstance *ci, CMPICount i,
                             char **name, CMPIStatus *rc, int readonly,
                             unsigned long* quals)
{
  ClInstance     *inst = (ClInstance *) ci->hdl;
  CMPIData        rv = { 0, CMPI_notFound, {0} };
  if (ClInstanceGetPropertyAt(inst, i, &rv, name, quals)) {
    if (rc)
      CMSetStatus(rc, CMPI_RC_ERR_NO_SUCH_PROPERTY);
    return rv;
  }

  if (rv.type == CMPI_chars) {
    rv.value.string =
        sfcb_native_new_CMPIString(rv.value.chars, NULL, readonly ? 2 : 0);
    rv.type = CMPI_string;
  } else if (readonly == 0 && rv.type == CMPI_string) {
    /*
     * Not read-only - must make a managed copy 
     */
    rv.value.string =
        sfcb_native_new_CMPIString(rv.value.string->hdl, NULL, 0);
  } else if (rv.type == CMPI_ref) {
    rv.value.ref = getObjectPath((char *)
                                 ClObjectGetClString(&inst->hdr,
                                                     (ClString *) &
                                                     rv.value.chars),
                                 NULL);
  } else if (rv.type & CMPI_ARRAY && rv.value.array) {
    rv.value.array =
        native_make_CMPIArray((CMPIData *) rv.value.array, NULL,
                              &inst->hdr);
  }

  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);
  return rv;
}

CMPIData
__ift_getPropertyAt(const CMPIInstance *ci, CMPICount i,
                    CMPIString **name, CMPIStatus *rc)
{
  CMPIData rv = { 0, CMPI_notFound, {0} };
  char           *sname;

  if (!ci->hdl) {
    if (rc)
      CMSetStatus(rc, CMPI_RC_ERR_INVALID_HANDLE);
    return rv;
  }

  rv = __ift_internal_getPropertyAt(ci, i, &sname, rc, 0, NULL);
  if (name) {
    *name = sfcb_native_new_CMPIString(sname, NULL, 0);
  }
  return rv;
}

CMPIData
__ift_getProperty(const CMPIInstance *ci, const char *id, CMPIStatus *rc)
{
  ClInstance     *inst;
  ClSection      *prps;
  CMPIData        rv = { 0, CMPI_notFound, {0} };
  int             i;

  if (!ci->hdl) {
    if (rc)
      CMSetStatus(rc, CMPI_RC_ERR_INVALID_HANDLE);
    return rv;
  } else if (!id) {
    if (rc)
      CMSetStatus(rc, CMPI_RC_ERR_NO_SUCH_PROPERTY);
    return rv;
  }

  inst = (ClInstance *) ci->hdl;
  prps = &inst->properties;

  if ((i = ClObjectLocateProperty(&inst->hdr, prps, id)) != 0) {
    return __ift_getPropertyAt(ci, i - 1, NULL, rc);
  }

  if (rc)
    CMSetStatus(rc, CMPI_RC_ERR_NO_SUCH_PROPERTY);
  return rv;
}

static CMPICount
__ift_getPropertyCount(const CMPIInstance *ci, CMPIStatus *rc)
{
  ClInstance     *inst = (ClInstance *) ci->hdl;

  if (!ci->hdl) {
    if (rc)
      CMSetStatus(rc, CMPI_RC_ERR_INVALID_HANDLE);
    return (CMPICount) 0;
  }

  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);
  return (CMPICount) ClInstanceGetPropertyCount(inst);
}

static CMPIStatus __ift_addPropertyQualifier(const CMPIInstance * instance,
                                             const char *name,
                                             const char *qualifier )
{
  ClInstance *inst;
  int rc;

  if (!instance->hdl) {
    CMReturn(CMPI_RC_ERR_INVALID_HANDLE);
  }

  inst = (ClInstance *) instance->hdl;
  rc = ClInstanceAddPropertyQualifierSpecial(inst, name, qualifier);
  CMReturn(rc);
}

static CMPIStatus
__ift_setProperty(const CMPIInstance *instance,
                  const char *name, const CMPIValue * value, CMPIType type)
{
  struct native_instance *i = (struct native_instance *) instance;
  ClInstance     *inst;
  CMPIData        data = { type, CMPI_goodValue, {0LL} };
  int             rc;

  if (!instance->hdl) {
    CMReturn(CMPI_RC_ERR_INVALID_HANDLE);
  }

  inst = (ClInstance *) instance->hdl;

  if (type == CMPI_chars) {
    /*
     * VM: is this OK or do we need a __new copy 
     */
    data.value.chars = (char *) value;
  } else if (type == CMPI_string) {
    data.type = CMPI_chars;
    if (value && value->string && value->string->hdl) {
      /*
       * VM: is this OK or do we need a __new copy 
       */
      data.value.chars = (char *) value->string->hdl;
    } else {
      data.value.chars = NULL;
    }
  } else if (value) {
    sfcb_setAlignedValue(&data.value, value, type);
  }

  if (((type & CMPI_ENCA) && data.value.chars == NULL) || value == NULL) {
    data.state = CMPI_nullValue;
  }

  if (i->filtered == 0 ||
      i->property_list == NULL ||
      __contained_list(i->property_list, name) ||
      __contained_list(i->key_list, name)) {

    rc = ClInstanceAddProperty(inst, name, data);

    if (i->filtered && !__contained_list(i->property_list, name)
        && __contained_list(i->key_list, name)) {
      /*
       * rc is the number of properties used, so we have to substract one
       * here 
       */
      ClInstanceFilterFlagProperty(inst, rc - 1);
    }
    if (rc < 0)
      /* negative rc is a negated CMPI_RC_ERR return code */
      CMReturn(-rc);
  }
  CMReturn(CMPI_RC_OK);
}

CMPIStatus
filterFlagProperty(CMPIInstance* ci, const char* id) {

  CMPIStatus     st = { CMPI_RC_OK, 0 };
  ClInstance     *inst = (ClInstance *) ci->hdl;
  ClSection      *prps = &inst->properties;
  int             i;

  if ((i = ClObjectLocateProperty(&inst->hdr, prps, id)) != 0) {
      ClInstanceFilterFlagProperty(inst, i-1);
  }
  else
    st.rc = CMPI_RC_ERR_NOT_FOUND;

  return st;
}

static CMPIStatus
__ift_setObjectPath(CMPIInstance *inst, const CMPIObjectPath * cop)
{
  CMPIStatus      tmp1,
                  tmp2,
                  tmp3;
  CMPIString     *str;
  const char     *ns,
                 *cn;
  int             j;
  CMPIStatus      rc = { CMPI_RC_OK, NULL };

  if (!inst->hdl) {
    CMSetStatus(&rc, CMPI_RC_ERR_INVALID_HANDLE);
    return rc;
  }

  /* in the case the instance is filtered need to reset the filter with new key list */
  if (((struct native_instance *)inst)->filtered) {
    char **props = ((struct native_instance *)inst)->property_list;
    char **keys = __make_key_list(cop);
    __ift_internal_setPropertyFilter(inst, (const char **)props, (const char **)keys);
    __release_list(keys);
  }

  /*
   * get information out of objectpath 
   */
  if (cop) {
    j = CMGetKeyCount(cop, &tmp1);
    str = CMGetClassName(cop, &tmp2);
    cn = CMGetCharsPtr(str, NULL);
    str = CMGetNameSpace(cop, &tmp3);
    ns = CMGetCharsPtr(str, NULL);
  }

  /*
   * there SHOULD be an op passed in, otherwise this call is useless.... 
   */
  else {
    j = 0;
    // SFCB_ASM("int $3");
    ns = "*NoNameSpace*";
    cn = "*NoClassName*";
    tmp1.rc = tmp2.rc = tmp3.rc = CMPI_RC_OK;
    tmp1.msg = tmp2.msg = tmp3.msg = NULL;
  }

  if (tmp1.rc != CMPI_RC_OK || tmp2.rc != CMPI_RC_OK
      || tmp3.rc != CMPI_RC_OK) {
    rc.rc = CMPI_RC_ERR_FAILED;
    return rc;
  } else {

    /*
     * set cn and ns in inst 
     */
    ClInstance     *instance = (ClInstance *) inst->hdl;
    if (ns)
      instance->nameSpace.id = addClString(&instance->hdr, ns);
    if (cn)
      instance->className.id = addClString(&instance->hdr, cn);

    /*
     * loop over keys, set them in the inst 
     */
    while (j-- && (tmp1.rc == CMPI_RC_OK)) {
      CMPIString     *keyName;
      CMPIData        tmp = CMGetKeyAt(cop, j, &keyName, &tmp1);
      __ift_setProperty(inst, CMGetCharsPtr(keyName, NULL),
                        &tmp.value, tmp.type);
    }
    return tmp1;
  }

  return rc;

}

static CMPIObjectPath *
__ift_getObjectPath(const CMPIInstance *instance, CMPIStatus *rc)
{
  static UtilHashTable *klt = NULL;
  int             j,
                  f = 0;
  CMPIStatus      tmp;
  const char     *cn;
  const char     *ns;

  if (!instance->hdl) {
    if (rc)
      CMSetStatus(rc, CMPI_RC_ERR_INVALID_HANDLE);
    return NULL;
  }

  cn = ClInstanceGetClassName((ClInstance *) instance->hdl);
  ns = ClInstanceGetNameSpace((ClInstance *) instance->hdl);

  CMPIObjectPath *cop;
  cop = TrackedCMPIObjectPath(ns, cn, rc);

  if (rc && rc->rc != CMPI_RC_OK)
    return NULL;

  j = __ift_getPropertyCount(instance, NULL);

  while (j--) {
    char           *keyName;
    CMPIData        d =
      __ift_internal_getPropertyAt(instance, j, &keyName, &tmp, 1, NULL);
    if (d.state & CMPI_keyValue) {
      CMAddKey(cop, keyName, &d.value, d.type);
      f++;
    }
    if (d.type & CMPI_ARRAY && (d.state & CMPI_nullValue) == 0) {
      d.value.array->ft->release(d.value.array);
    }
  }

  if (f == 0) {
    CMPIArray      *kl;
    CMPIData        d;
    unsigned int    e,
                    m;

    pthread_mutex_lock(&gopMtx);
    if (klt == NULL)
      klt = UtilFactory->newHashTable(61,
                                      UtilHashTable_charKey |
                                      UtilHashTable_ignoreKeyCase);

    if ((kl = klt->ft->get(klt, cn)) == NULL) {
      CMPIConstClass *cc = getConstClass(ns, cn);
      if (cc) {
        kl = cc->ft->getKeyList(cc);
        klt->ft->put(klt, strdup(cn), kl);
      } else {
        if (rc) {
          CMSetStatus(rc, CMPI_RC_ERR_INVALID_CLASS);
        }
        pthread_mutex_unlock(&gopMtx);
        return NULL;
      }
    }
    pthread_mutex_unlock(&gopMtx);
    m = kl->ft->getSize(kl, NULL);

    for (e = 0; e < m; e++) {
      CMPIString     *n = kl->ft->getElementAt(kl, e, NULL).value.string;
      d = __ift_getProperty(instance, CMGetCharPtr(n), &tmp);
      if (tmp.rc == CMPI_RC_OK) {
        CMAddKey(cop, CMGetCharPtr(n), &d.value, d.type);
      }
    }
  }
  return cop;
}

/* same as ift_gOP(), but used for SfcbLocal clients
   eliminated klt hashtable, call mm to release tracked
   CMPI objects before exiting */
static CMPIObjectPath *
__iftLocal_getObjectPath(const CMPIInstance *instance, CMPIStatus *rc)
{
  int             j,
                  f = 0;
  CMPIStatus      tmp;
  const char     *cn;
  const char     *ns;
  void*           hc;

  if (!instance->hdl) {
    if (rc)
      CMSetStatus(rc, CMPI_RC_ERR_INVALID_HANDLE);
    return NULL;
  }

  cn = ClInstanceGetClassName((ClInstance *) instance->hdl);
  ns = ClInstanceGetNameSpace((ClInstance *) instance->hdl);

  CMPIObjectPath *cop = TrackedCMPIObjectPath(ns, cn, rc);

  if (rc && rc->rc != CMPI_RC_OK)
    return NULL;

  j = __ift_getPropertyCount(instance, NULL);

  hc = markHeap();
  while (j--) {
    char           *keyName;
    CMPIData        d =
      __ift_internal_getPropertyAt(instance, j, &keyName, &tmp, 1, NULL);
    if (d.state & CMPI_keyValue) {
      CMAddKey(cop, keyName, &d.value, d.type);
      f++;
    }
    if (d.type & CMPI_ARRAY && (d.state & CMPI_nullValue) == 0) {
      d.value.array->ft->release(d.value.array);
    }
  }

  if (f == 0) {
    CMPIArray      *kl;
    CMPIData        d;
    unsigned int    e,
                    m;

    CMPIConstClass *cc = getConstClass(ns, cn);
    if (cc) {
      kl = cc->ft->getKeyList(cc);
    } else {
      if (rc) {
	CMSetStatus(rc, CMPI_RC_ERR_INVALID_CLASS);
      }
      releaseHeap(hc);
      return NULL;
    }
    m = kl->ft->getSize(kl, NULL);

    for (e = 0; e < m; e++) {
      CMPIString     *n = kl->ft->getElementAt(kl, e, NULL).value.string;
      d = __ift_getProperty(instance, CMGetCharPtr(n), &tmp);
      if (tmp.rc == CMPI_RC_OK) {
        CMAddKey(cop, CMGetCharPtr(n), &d.value, d.type);
      }
    }
    CMRelease(kl);
  }
  releaseHeap(hc);
  return cop;
}

static CMPIStatus
__ift_internal_setPropertyFilter(CMPIInstance *instance,
                                 const char **propertyList, const char **keys)
{
  int             j,
                  m;
  CMPIObjectPath *cop;
  CMPIInstance   *newInstance;
  CMPIStatus      st;
  CMPIData        data;
  char           *name;
  struct native_instance *i = (struct native_instance *) instance;
  struct native_instance *iNew,
                  iTemp;

  cop =
      TrackedCMPIObjectPath(instGetNameSpace(instance),
                            instGetClassName(instance), NULL);
  if (cop) {
    if (i->mem_state == MEM_RELEASED || i->mem_state > 0) {
      newInstance = internal_new_CMPIInstance(MEM_TRACKED, cop, &st, 1);
    } else {
      newInstance =
          internal_new_CMPIInstance(MEM_NOT_TRACKED, cop, &st, 1);
    }
    iNew = (struct native_instance *) newInstance;
    iNew->filtered = 1;
    iNew->property_list = __duplicate_list(propertyList);
    iNew->key_list = __duplicate_list(keys);
    for (j = 0, m = __ift_getPropertyCount(instance, &st); j < m; j++) {
      data = __ift_internal_getPropertyAt(instance, j, &name, &st, 1, NULL);
      if (__contained_list((char **) propertyList, name)
          || __contained_list((char **) keys, name)) {
        if ((data.state & ~CMPI_keyValue) != 0) {
          newInstance->ft->setProperty(newInstance, name, NULL, data.type);
        } else {
          newInstance->ft->setProperty(newInstance, name, &data.value,
                                       data.type);
        }
      }
    }

    if (i->mem_state == MEM_RELEASED) {
      memcpy(i, iNew, sizeof(struct native_instance));
    } else {
      memcpy(&iTemp, i, sizeof(struct native_instance));
      memcpy(i, iNew, sizeof(struct native_instance));
      i->refCount = iTemp.refCount;
      memcpy(iNew, &iTemp, sizeof(struct native_instance));
      if (iNew->mem_state > 0) {
        iNew->mem_state = i->mem_state;
        i->mem_state = iTemp.mem_state;
      } else {
        newInstance->ft->release(newInstance);
      }
    }
  }

  CMReturn(CMPI_RC_OK);
}

static CMPIStatus __ift_setPropertyFilter(CMPIInstance * instance,
                                          const char **propertyList,
                                          const char __attribute__ ((unused)) **keys)
{
  CMPIStatus rc = { CMPI_RC_OK, NULL};
  CMPIObjectPath *cop = NULL;
  char **ikeys = NULL;

  if (propertyList == NULL) {
    /* NULL property list, no need to set filter */
    CMReturn(CMPI_RC_OK);
  }
  if (!instance->hdl) {
    rc.rc = CMPI_RC_ERR_INVALID_HANDLE;
  }
  else {
    /* CMPI 2.0 dictates that keyList is to be ignored by MB, and remains for Binary compat.
       Build keyList from instance objectpath to be passed to internal filter implementation */
    cop = CMGetObjectPath(instance, NULL);
    ikeys = __make_key_list(cop);

    rc = __ift_internal_setPropertyFilter(instance, propertyList, (const char **)ikeys);

    __release_list(ikeys);
  }

  return rc;
}

static CMPIData
__ift_getQualifier(CMPIInstance *inst, const char *name, CMPIStatus *rc)
{
  CMPIData        data = { 0, CMPI_notFound, {0} };
  if (rc)
    CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
  return data;
}

static CMPIData
__ift_getQualifierAt(CMPIInstance *inst, unsigned int index,
                     CMPIString **name, CMPIStatus *rc)
{
  CMPIData        data = { 0, CMPI_notFound, {0} };
  if (rc)
    CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
  return data;
}

static unsigned int
__ift_getQualifierCount(CMPIInstance *inst, CMPIStatus *rc)
{
  if (rc)
    CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
  return 0;
}

static CMPIData
__ift_getPropertyQualifier(CMPIInstance *inst, const char *pname,
                           const char *qname, CMPIStatus *rc)
{
  CMPIData        data = { 0, CMPI_notFound, {0} };
  if (rc)
    CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
  return data;
}

static CMPIData
__ift_getPropertyQualifierAt(CMPIInstance *inst, const char *pname,
                             unsigned int index, CMPIString **name,
                             CMPIStatus *rc)
{
  CMPIData        data = { 0, CMPI_notFound, {0} };
  if (rc)
    CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
  return data;
}

static unsigned int
__ift_getPropertyQualifierCount(CMPIInstance *inst, const char *pname,
                                CMPIStatus *rc)
{
  if (rc)
    CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
  return 0;
}

void
add(char **buf, unsigned int *p, unsigned int *m, char *data)
{
  unsigned int    ds = strlen(data) + 1;

  if (*buf == NULL) {
    *buf = malloc(1024);
    *p = 0;
    *m = 1024;
  }
  if ((ds + (*p)) >= *m) {
    unsigned        nm = *m;
    char           *nb;
    while ((ds + (*p)) >= nm)
      nm *= 2;
    nb = malloc(nm);
    memcpy(nb, *buf, *p);
    free(*buf);
    *buf = nb;
    *m = nm;
  }
  memcpy(*buf + (*p), data, ds);
  *p += ds - 1;
}

extern char    *sfcb_value2Chars(CMPIType type, CMPIValue * value);

CMPIString     *
instance2String(CMPIInstance *inst, CMPIStatus *rc)
{
  CMPIObjectPath *path;
  CMPIData        data;
  CMPIString     *name,
                 *ps,
                 *rv;
  unsigned int    i,
                  m;
  char           *buf = NULL,
      *v,
      *pname;
  unsigned int    bp,
                  bm;

  add(&buf, &bp, &bm, "Instance of ");
  path = __ift_getObjectPath(inst, NULL);
  name = path->ft->toString(path, rc);
  add(&buf, &bp, &bm, (char *) name->hdl);
  add(&buf, &bp, &bm, " {\n");
  ps = path->ft->toString(path, rc);
  add(&buf, &bp, &bm, " PATH: ");
  add(&buf, &bp, &bm, (char *) ps->hdl);
  add(&buf, &bp, &bm, "\n");

  for (i = 0, m = __ift_getPropertyCount(inst, rc); i < m; i++) {
    data = __ift_internal_getPropertyAt(inst, i, &pname, rc, 1, NULL);
    add(&buf, &bp, &bm, " ");
    add(&buf, &bp, &bm, pname);
    add(&buf, &bp, &bm, " = ");
    v = sfcb_value2Chars(data.type, &data.value);
    add(&buf, &bp, &bm, v);
    free(v);
    add(&buf, &bp, &bm, " ;\n");
  }
  add(&buf, &bp, &bm, "}\n");
  rv = sfcb_native_new_CMPIString(buf, rc, 1);
  return rv;
}

static CMPIInstanceFT ift = {
  NATIVE_FT_VERSION,
  __ift_release,
  __ift_clone,
  __ift_getProperty,
  __ift_getPropertyAt,
  __ift_getPropertyCount,
  __ift_setProperty,
  __ift_getObjectPath,
  __ift_setPropertyFilter,
  __ift_setObjectPath
};

static struct {
  unsigned int    ftVersion;
  CMPIStatus      (*release)
                  (CMPIInstance *inst);
  CMPIInstance   *(*clone) (const CMPIInstance *inst, CMPIStatus *rc);
  CMPIData        (*getProperty) (const CMPIInstance *inst,
                                  const char *name, CMPIStatus *rc);
  CMPIData        (*getPropertyAt) (const CMPIInstance *inst,
                                    unsigned int index, CMPIString **name,
                                    CMPIStatus *rc);
  unsigned int    (*getPropertyCount) (const CMPIInstance *inst,
                                       CMPIStatus *rc);
  CMPIStatus      (*setProperty) (const CMPIInstance *inst,
                                  const char *name,
                                  const CMPIValue * value, CMPIType type);
  CMPIObjectPath *(*getObjectPath) (const CMPIInstance *inst,
                                    CMPIStatus *rc);
  CMPIStatus      (*setPropertyFilter) (CMPIInstance *inst,
                                        const char **propertyList,
                                        const char **keys);
  CMPIData        (*getQualifier) (CMPIInstance *inst, const char *name,
                                   CMPIStatus *rc);
  CMPIData        (*getQualifierAt) (CMPIInstance *inst,
                                     unsigned int index, CMPIString **name,
                                     CMPIStatus *rc);
  unsigned int    (*getQualifierCount) (CMPIInstance *inst,
                                        CMPIStatus *rc);
  CMPIData        (*getPropertyQualifier) (CMPIInstance *inst,
                                           const char *pname,
                                           const char *qname,
                                           CMPIStatus *rc);
  CMPIData        (*getPropertyQualifierAt) (CMPIInstance *inst,
                                             const char *pname,
                                             unsigned int index,
                                             CMPIString **name,
                                             CMPIStatus *rc);
  unsigned int    (*getPropertyQualifierCount) (CMPIInstance *inst,
                                                const char *pname,
                                                CMPIStatus *rc);

} iftLocal = {
NATIVE_FT_VERSION,
      __ift_release,
      __ift_clone,
      __ift_getProperty,
      __ift_getPropertyAt,
      __ift_getPropertyCount,
      __ift_setProperty,
      __iftLocal_getObjectPath,
      __ift_setPropertyFilter,
      __ift_getQualifier,
      __ift_getQualifierAt,
      __ift_getQualifierCount,
      __ift_getPropertyQualifier,
      __ift_getPropertyQualifierAt, __ift_getPropertyQualifierCount};

CMPIInstanceFT *CMPI_Instance_FT = &ift;

unsigned long
getInstanceSerializedSize(const CMPIInstance *ci)
{
  ClInstance     *cli = (ClInstance *) ci->hdl;
  return ClSizeInstance(cli) + sizeof(struct native_instance);
}

void
getSerializedInstance(const CMPIInstance *ci, void *area)
{
  memcpy(area, ci, sizeof(struct native_instance));
  ClInstanceRebuild((ClInstance *) ci->hdl,
                    (void *) ((char *) area +
                              sizeof(struct native_instance)));
  ((CMPIInstance *) (area))->hdl =
      (ClInstance *) ((char *) area + sizeof(struct native_instance));
}

CMPIInstance   *
relocateSerializedInstance(void *area)
{
  struct native_instance *ci = (struct native_instance *) area;
  ci->instance.hdl = ci + 1;
  ci->instance.ft = CMPI_Instance_FT;
  ci->mem_state = MEM_RELEASED;
  ci->property_list = NULL;
  ci->key_list = NULL;
  ClInstanceRelocateInstance((ClInstance *) ci->instance.hdl);
  return (CMPIInstance *) ci;
}

MsgSegment
setInstanceMsgSegment(CMPIInstance *ci)
{
  MsgSegment      s;
  s.data = ci;
  s.type = MSG_SEG_INSTANCE;
  s.length = getInstanceSerializedSize(ci);
  return s;
}

CMPIInstance   *
internal_new_CMPIInstance(int mode, const CMPIObjectPath * cop,
                          CMPIStatus *rc, int override)
{
  CMPIInstance    i = {
    "CMPIInstance",
    CMPI_Instance_FT
  };

  struct native_instance instance,
                 *tInst;
  memset(&instance, 0, sizeof(instance));

  CMPIStatus      tmp1,
                  tmp2,
                  tmp3;
  CMPIString     *str;
  const char     *ns,
                 *cn;
  int             j,
                  state;

  instance.instance = i;

  if (cop) {
    j = CMGetKeyCount(cop, &tmp1);
    str = CMGetClassName(cop, &tmp2);
    cn = CMGetCharsPtr(str, NULL);
    str = CMGetNameSpace(cop, &tmp3);
    ns = CMGetCharsPtr(str, NULL);
  }

  else {
    j = 0;
    // SFCB_ASM("int $3");
    ns = "*NoNameSpace*";
    cn = "*NoClassName*";
    tmp1.rc = tmp2.rc = tmp3.rc = CMPI_RC_OK;
  }

  if (tmp1.rc != CMPI_RC_OK || tmp2.rc != CMPI_RC_OK
      || tmp3.rc != CMPI_RC_OK) {
    if (rc)
      CMSetStatus(rc, CMPI_RC_ERR_FAILED);
  } else {
    instance.instance.hdl = ClInstanceNew(ns, cn);

#ifdef HAVE_DEFAULT_PROPERTIES
    if (!override) {
      if (instFillDefaultProperties(&instance,ns,cn)) {
         mlogf(M_ERROR, M_SHOW, "--- Could not fill default properties for instance of %ss; mutex creation failure\n", cn);      
      }
    }
#endif

    while (j-- && (tmp1.rc == CMPI_RC_OK)) {
      CMPIString     *keyName;
      CMPIData        tmp = CMGetKeyAt(cop, j, &keyName, &tmp1);
      __ift_setProperty(&instance.instance, CMGetCharsPtr(keyName, NULL),
                        &tmp.value, tmp.type);
    }
    if (rc)
      CMSetStatus(rc, tmp1.rc);
  }

  tInst = memAddEncObj(mode, &instance, sizeof(instance), &state);
  tInst->mem_state = state;
  tInst->refCount = 0;

  return (CMPIInstance *) tInst;
}

CMPIInstance   *
TrackedCMPIInstance(const CMPIObjectPath * cop, CMPIStatus *rc)
{
  return internal_new_CMPIInstance(MEM_TRACKED, cop, rc, 0);
}

CMPIInstance   *
NewCMPIInstance(CMPIObjectPath * cop, CMPIStatus *rc)
{
  return internal_new_CMPIInstance(MEM_NOT_TRACKED, cop, rc, 0);
}

const char     *
instGetClassName(CMPIInstance *ci)
{
  ClInstance     *inst = (ClInstance *) ci->hdl;
  return ClInstanceGetClassName(inst);
}

const char     *
instGetNameSpace(CMPIInstance *ci)
{
  ClInstance     *inst = (ClInstance *) ci->hdl;
  return ClInstanceGetNameSpace(inst);
}

int
instanceCompare(CMPIInstance *inst1, CMPIInstance *inst2)
{
  unsigned int    c,
                  i;
  CMPIStatus      st = { CMPI_RC_OK, NULL };
  CMPIData        d1,
                  d2;
  CMPIString     *propName;

  /*
   * check if we have null pointers for our instances 
   */
  if (inst1 == NULL && inst2 == NULL)
    return 0;                   /* identical */
  if (inst1 == NULL)
    return -1;                  /* inst1 is less than inst2 */
  if (inst2 == NULL)
    return 1;                   /* inst2 is less than inst1 */

  c = inst1->ft->getPropertyCount(inst1, NULL);

  if (c != inst2->ft->getPropertyCount(inst2, NULL)) {
    /*
     * property count not equal, instances cannot be identical 
     */
    return 1;
  }

  for (i = 0; i < c; i++) {
    /*
     * get property at position i, store the name in propName 
     */
    d1 = inst1->ft->getPropertyAt(inst1, i, &propName, NULL);
    /*
     * try to find the property in instance2 via the name just retrieved 
     */
    d2 = inst2->ft->getProperty(inst2,
                                propName->ft->getCharPtr(propName, NULL),
                                &st);

    if (st.rc ||                /* property could not be retrieved,
                                 * probably non existent */
        d1.type != d2.type ||   /* types differ */
        sfcb_comp_CMPIValue(&d1.value, &d2.value, d1.type)) {   /* values
                                                                 * differ */

      /*
       * instances not equal 
       */
      return 1;
    }
  }
  /*
   * passed all tests - instances seem to be identical 
   */
  return 0;
}

/*
 * needed for local client support as function tables differ 
 */
void
setInstanceLocalMode(int mode)
{
  if (mode) {
    CMPI_Instance_FT = (CMPIInstanceFT *) &iftLocal;
  } else {
    CMPI_Instance_FT = &ift;
  }
}

#ifdef HAVE_DEFAULT_PROPERTIES
static int
instFillDefaultProperties(struct native_instance *inst,
                          const char *ns, const char *cn)
{
  static UtilHashTable *clt = NULL;
  CMPIConstClass *cc;
  CMPICount       pc;
  CMPIData        pd;
  CMPIStatus      ps;
  CMPIString     *pn = NULL;
  CMPIValue      *vp;

  pthread_mutex_lock(&ifdpMtx);
  if (clt == NULL)
    clt =
        UtilFactory->newHashTable(61,
                                  UtilHashTable_charKey |
                                  UtilHashTable_ignoreKeyCase);

  if ((cc = clt->ft->get(clt, cn)) == NULL) {
    cc = getConstClass(ns, cn);
    if (cc) {
      clt->ft->put(clt, strdup(cn), cc->ft->clone(cc, NULL));
    }
  }
  pthread_mutex_unlock(&ifdpMtx);
  if (cc) {
    pc = cc->ft->getPropertyCount(cc, NULL);
    while (pc > 0) {
      pc -= 1;
      pd = cc->ft->getPropertyAt(cc, pc, &pn, &ps);

      /* if this prop is an EmbeddedObject, force type to CMPI_instance to allow CMSetProperty with a CMPI_Instance */
      /* (also works for EmbeddedInstance, since the EmbeddedObject qual will also be set in that case */
      CMPIData pqd = cc->ft->getPropQualifier(cc, CMGetCharsPtr(pn, NULL), "EmbeddedObject", NULL);
      if ((pqd.state == CMPI_goodValue) && (pqd.value.boolean == 1)) {
	pd.type = CMPI_instance;
      }
      
      if (ps.rc == CMPI_RC_OK && pn) {
        vp = &pd.value;
        if (pd.state & CMPI_nullValue) {
          /*
           * must set null value indication: CMPI doesn't allow to do
           * that properly 
           */
          pd.value.chars = NULL;
          if ((pd.type & (CMPI_SIMPLE | CMPI_REAL | CMPI_INTEGER)) &&
              (pd.type & CMPI_ARRAY) == 0) {
            vp = NULL;
          }
        }
        __ift_setProperty(&inst->instance, CMGetCharsPtr(pn, NULL),
                          vp, pd.type);

        /* Copy EmbeddedInstance qualifier from the class to the instance,
           so we know, what to put into CIM-XML */
        CMPIData pqd = cc->ft->getPropQualifier(cc, CMGetCharsPtr(pn, NULL), "EmbeddedInstance", NULL);
        if ((pqd.state == CMPI_goodValue) && (pqd.value.string != NULL)) {
          __ift_addPropertyQualifier(&inst->instance, CMGetCharsPtr(pn,NULL), "EmbeddedInstance");
        }

      }
    }
  }
  return 0;
}
#endif

/*
  Set the CreationClassName and SystemCreationClassName
  According to DSP1001, these need not be specified by the client
  and should be ignored if provided.
*/
void
setCCN(CMPIObjectPath * cop,
        CMPIInstance *ci,
        const char * sccn)
{
  CMPIString* ccn = CMGetClassName(cop, NULL);
  CMAddKey(cop, "creationclassname", CMGetCharPtr(ccn), CMPI_chars);
  CMSetProperty(ci,"creationclassname", CMGetCharPtr(ccn), CMPI_chars);
  CMAddKey(cop, "systemcreationclassname", sccn, CMPI_chars);
  CMSetProperty(ci,"systemcreationclassname", sccn, CMPI_chars);
}

/****************************************************************************/
/* MODELINES */
/* DO NOT EDIT BELOW THIS COMMENT */
/* Modelines are added by 'make pretty' */
/* -*- Mode: C; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* vi:set ts=2 sts=2 sw=2 expandtab: */
