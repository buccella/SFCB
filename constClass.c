
/*
 * constClass.c
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
 * Author:       Adrian Schuur <schuur@de.ibm.com>
 *
 * Description:
 *
 * Internal constClass support for sfcb .
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constClass.h"
#include "objectImpl.h"
#include "msgqueue.h"

// #define DEB(x) x
#define DEB(x)

extern CMPIArray *native_make_CMPIArray(CMPIData *av, CMPIStatus *rc,
                                        ClObjectHdr * hdr);
extern CMPIObjectPath *getObjectPath(char *path, char **msg);

unsigned long   getConstClassSerializedSize(CMPIConstClass * cl);
static CMPIConstClass *cls_clone(CMPIConstClass * cc, CMPIStatus *rc);

static CMPIStatus
release(CMPIConstClass * cc)
{
  CMPIStatus      rc = { 0, NULL };

  if (cc->refCount == 0) {
    if (cc->hdl) {
      if (cc->hdl != (void *) (cc + 1))
        ClClassFreeClass(cc->hdl);
    }
    free(cc);
  }
  return rc;
}

static void
relocate(CMPIConstClass * cc)
{
  ClClassRelocateClass((ClClass *) cc->hdl);
}

static const char *
getCharClassName(const CMPIConstClass * cc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  if (cls->name.id)
    return ClObjectGetClString(&cls->hdr, &cls->name);
  return NULL;
}

static CMPIString *
getClassName(CMPIConstClass * cc, CMPIStatus *rc)
{
  const char     *cn = getCharClassName(cc);
  return sfcb_native_new_CMPIString(cn, rc, 0);
}

static const char *
getCharSuperClassName(const CMPIConstClass * cc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  if (cls->parent.id)
    return ClObjectGetClString(&cls->hdr, &cls->parent);
  return NULL;
}

static CMPIString *
getSuperClassName(CMPIConstClass * cc, CMPIStatus *rc)
{
  const char     *cn = getCharSuperClassName(cc);
  return sfcb_native_new_CMPIString(cn, rc, 0);
}

CMPIBoolean
isAbstract(CMPIConstClass * cc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  return (cls->quals & ClClass_Q_Abstract) != 0;
}

static CMPIBoolean
isIndication(CMPIConstClass * cc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  return (cls->quals & ClClass_Q_Indication) != 0;
}

static CMPIBoolean
isAssociation(CMPIConstClass * cc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  return (cls->quals & ClClass_Q_Association) != 0;
}

static char    *
toString(CMPIConstClass * cc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  return ClClassToString(cls);
}

static CMPICount
getPropertyCount(CMPIConstClass * cc, CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);
  return (CMPICount) ClClassGetPropertyCount(cls);
}

CMPIData
getPropertyQualsAt(CMPIConstClass * cc, CMPICount i, CMPIString **name,
                   unsigned long *quals, CMPIString **refName,
                   CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  char           *n;
  char           *rName = NULL;
  CMPIData        rv = { 0, CMPI_notFound, {0} };
  if (ClClassGetPropertyAt(cls, i, &rv, name ? &n : NULL, quals, &rName)) {
    if (rc)
      CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
    return rv;
  }
  if (rv.type == CMPI_chars) {
    rv.value.string = sfcb_native_new_CMPIString(rv.value.chars, NULL, 0);
    rv.type = CMPI_string;
  } else if (rv.type == CMPI_ref) {
    if ((rv.state & CMPI_nullValue) == 0)
      rv.value.ref = getObjectPath((char *)
                                   ClObjectGetClString(&cls->hdr,
                                                       (ClString *) &
                                                       rv.value.chars),
                                   NULL);
  }
  if (rv.type & CMPI_ARRAY && rv.value.array) {
    rv.value.array =
        native_make_CMPIArray((CMPIData *) rv.value.array, NULL,
                              &cls->hdr);
  }
  if (name) {
    *name = sfcb_native_new_CMPIString(n, NULL, 0);
  }
  if (refName && rName) {
    *refName = sfcb_native_new_CMPIString(rName, NULL, 0);
  } else {
    if (refName) {
      *refName = NULL;
    }
  }
  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);
  return rv;
}

static CMPIData
getPropertyAt(CMPIConstClass * cc, CMPICount i, CMPIString **name,
              CMPIStatus *rc)
{
  return getPropertyQualsAt(cc, i, name, NULL, NULL, rc);
}

static CMPIData
getQualifier(CMPIConstClass * cc, const char *name, CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  CMPIData        rv_notfound = { 0, CMPI_notFound, {0} };
  CMPIData        rv;
  char           *qname;
  CMPICount       cnt = ClClassGetQualifierCount(cls),
                  i;

  for (i = 0; i < cnt; i++) {
    if (ClClassGetQualifierAt(cls, i, &rv, &qname)) {
      if (rc)
        CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
      return rv_notfound;
    }
    if (strcasecmp(name, qname) == 0) {
      if (rv.type == CMPI_chars) {
        rv.value.string = sfcb_native_new_CMPIString(ClObjectGetClString
                                                     (&cls->hdr,
                                                      (ClString *) &
                                                      rv.value.chars),
                                                     NULL, 0);
        rv.type = CMPI_string;
      }
      if (rv.type & CMPI_ARRAY && rv.value.array) {
        rv.value.array = native_make_CMPIArray((CMPIData *) rv.value.array,
                                               NULL, &cls->hdr);
      }
      if (rc)
        CMSetStatus(rc, CMPI_RC_OK);
      return rv;
    };
  }
  if (rc)
    CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
  return rv_notfound;
}

static CMPIData
getQualifierAt(CMPIConstClass * cc, CMPICount i, CMPIString **name,
               CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  char           *n;
  CMPIData        rv = { 0, CMPI_notFound, {0} };
  if (ClClassGetQualifierAt(cls, i, &rv, name ? &n : NULL)) {
    if (rc)
      CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
    return rv;
  }
  if (rv.type == CMPI_chars) {
    rv.value.string = sfcb_native_new_CMPIString(ClObjectGetClString
                                                 (&cls->hdr,
                                                  (ClString *) & rv.value.
                                                  chars), NULL, 0);
    rv.type = CMPI_string;
  }
  if (rv.type & CMPI_ARRAY && rv.value.array) {
    rv.value.array = native_make_CMPIArray((CMPIData *) rv.value.array,
                                           NULL, &cls->hdr);
  }
  if (name) {
    *name = sfcb_native_new_CMPIString(n, NULL, 0);
  }
  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);
  return rv;
}

static CMPICount
getQualifierCount(CMPIConstClass * cc, CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);
  return (CMPICount) ClClassGetQualifierCount(cls);
}

CMPIData
internalGetMethQualAt(CMPIConstClass * cc, CMPICount m, CMPICount i,
                      CMPIString **name, CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  char           *n;
  CMPIData        rv = { 0, CMPI_notFound, {0} };

  ClMethod *mthd = (ClMethod *) ClObjectGetClSection(&cls->hdr, &cls->methods);
  if (m > cls->methods.used)
    return rv;
  mthd = mthd + m;

  if (ClClassGetMethQualifierAt(cls, mthd, i, &rv, name ? &n : NULL)) {
    if (rc)
      CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
    if (name)
      *name = sfcb_native_new_CMPIString(NULL, NULL, 0);
    return rv;
  }
  if (rv.type == CMPI_chars) {
    rv.value.string = sfcb_native_new_CMPIString(ClObjectGetClString
                                                 (&cls->hdr,
                                                  (ClString *) & rv.value.
                                                  chars), NULL, 0);
    rv.type = CMPI_string;
  }
  if (name) {
    *name = sfcb_native_new_CMPIString(n, NULL, 0);
  }
  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);

  return rv;
}

CMPIData
internalGetMethParamAt(CMPIConstClass * cc, CMPICount m, CMPICount i,
                      CMPIString **name, CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  char           *n;
  CMPIData        rv = { 0, CMPI_notFound, {0} };
  CMPIParameter   p;

  ClMethod *mthd = (ClMethod *) ClObjectGetClSection(&cls->hdr, &cls->methods);
  if (m > cls->methods.used)
    return rv;
  mthd = mthd + m;

  if (ClClassGetMethParameterAt(cls, mthd, i, &p, name ? &n : NULL)) {
    if (rc)
      CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
    if (name)
      *name = sfcb_native_new_CMPIString(NULL, NULL, 0);
    return rv;
  }
  rv.type = p.type;
  if (name) {
    *name = sfcb_native_new_CMPIString(n, NULL, 0);
  }
  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);
  return rv;
}

CMPIData
internalGetPropQualAt(CMPIConstClass * cc, CMPICount p, CMPICount i,
                      CMPIString **name, CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  char           *n;
  CMPIData        rv = { 0, CMPI_notFound, {0} };

  if (ClClassGetPropQualifierAt(cls, p, i, &rv, name ? &n : NULL)) {
    if (rc)
      CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
    if (name)
      *name = sfcb_native_new_CMPIString(NULL, NULL, 0);
    return rv;
  }
  if (rv.type == CMPI_chars) {
    rv.value.string = sfcb_native_new_CMPIString(ClObjectGetClString
                                                 (&cls->hdr,
                                                  (ClString *) & rv.value.
                                                  chars), NULL, 0);
    rv.type = CMPI_string;
  }
  if (rv.type & CMPI_ARRAY && rv.value.dataPtr.ptr) {
    rv.value.array =
        native_make_CMPIArray((CMPIData *) rv.value.dataPtr.ptr, NULL,
                              &cls->hdr);
  }
  if (name) {
    *name = sfcb_native_new_CMPIString(n, NULL, 0);
  }
  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);
  return rv;
}

static CMPIData
getPropQualifierAt(CMPIConstClass * cc, const char *cp, CMPICount i,
                   CMPIString **name, CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  ClSection      *prps = &cls->properties;
  CMPICount       p = ClObjectLocateProperty(&cls->hdr, prps, cp) - 1;
  return internalGetPropQualAt(cc, p, i, name, rc);
}

static CMPIData
getPropQualifier(CMPIConstClass * cc, const char *cp, const char *cpq,
                 CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  ClSection      *prps = &cls->properties;
  char           *n = NULL;
  CMPIData        rv_notFound = { 0, CMPI_notFound, {0} };
  CMPIData        rv;
  CMPICount       p = ClObjectLocateProperty(&cls->hdr, prps, cp);
  CMPICount       num = ClClassGetPropQualifierCount(cls, p - 1);
  CMPICount       i;

  /*
   * special qualifier handling 
   */
  if (strcasecmp(cpq, "key") == 0) {
    unsigned long   quals;
    getPropertyQualsAt(cc, p - 1, NULL, &quals, NULL, rc);
    if (quals & ClProperty_Q_Key) {
      rv.type = CMPI_boolean;
      rv.state = CMPI_goodValue;
      rv.value.boolean = 1;
      if (rc)
        CMSetStatus(rc, CMPI_RC_OK);
      return rv;
    } else {
      if (rc) 
        CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
      return rv_notFound;
    }
  }
  if (strcasecmp(cpq, "embeddedobject") == 0) {
    unsigned long quals;
    getPropertyQualsAt(cc,p-1,NULL,&quals,NULL,rc);
    if (quals &  ClProperty_Q_EmbeddedObject) {
      rv.type = CMPI_boolean;
      rv.state = CMPI_goodValue;
      rv.value.boolean = 1;
      if (rc)
        CMSetStatus(rc, CMPI_RC_OK);
      return rv;
    } else {
      if (rc) 
        CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
      return rv_notFound;
    }
  }
  if (strcasecmp(cpq, "embeddedinstance") == 0) {
    unsigned long quals;
    getPropertyQualsAt(cc,p-1,NULL,&quals,NULL,rc);
    if (quals &  ClProperty_Q_EmbeddedInstance) {
      rv.type = CMPI_boolean;
      rv.state = CMPI_goodValue;
      rv.value.boolean = 1;
      if (rc) {
        CMSetStatus(rc, CMPI_RC_OK);
      }
      return rv;
    } else {
      if (rc) {
        CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
      }
      return rv_notFound;
    }
  }

  for (i = 0; i < num; i++) {
    if (ClClassGetPropQualifierAt(cls, p - 1, i, &rv, &n) == 0
        && strcasecmp(cpq, n) == 0) {
      if (rv.type == CMPI_chars) {
        rv.value.string = sfcb_native_new_CMPIString(ClObjectGetClString
                                                     (&cls->hdr,
                                                      (ClString *) &
                                                      rv.value.chars),
                                                     NULL, 0);
        rv.type = CMPI_string;
      } else if ((rv.type & CMPI_ARRAY) && rv.value.dataPtr.ptr) {
        rv.value.array =
            native_make_CMPIArray((CMPIData *) rv.value.dataPtr.ptr, NULL,
                                  &cls->hdr);
      }
      if (rc)
        CMSetStatus(rc, CMPI_RC_OK);
      return rv;
    }
    if (n && isMallocedStrBuf(&cls->hdr)) {
      free(n);
    }
  }
  if (rc) {
    CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
  }
  return rv_notFound;
}

static CMPICount
getPropQualifierCount(CMPIConstClass * cc, const char *prop,
                      CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  ClSection      *prps = &cls->properties;
  CMPICount       p = ClObjectLocateProperty(&cls->hdr, prps, prop);
  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);
  return ClClassGetPropQualifierCount(cls, p - 1);
}

CMPIData
getPropertyQuals(CMPIConstClass * cc, const char *id, unsigned long *quals,
                 CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  ClSection      *prps = &cls->properties;
  CMPIData        rv = { 0, CMPI_notFound, {0} };
  int             i;

  if ((i = ClObjectLocateProperty(&cls->hdr, prps, id)) != 0) {
    return getPropertyQualsAt(cc, i - 1, NULL, quals, NULL, rc);
  }

  if (rc)
    CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
  return rv;
}

static CMPIData
getProperty(CMPIConstClass * cc, const char *id, CMPIStatus *rc)
{
  return getPropertyQuals(cc, id, NULL, rc);
}

static CMPIArray *
getKeyList(CMPIConstClass * cc)
{
  int             i,
                  m,
                  c;
  unsigned long   quals;
  CMPIArray      *kar;
  CMPIString     *name;
  int             idx[32];

  for (i = c = 0, m = getPropertyCount(cc, NULL); i < m; i++) {
    getPropertyQualsAt(cc, i, NULL, &quals, NULL, NULL);
    if (quals & ClProperty_Q_Key) {
      idx[c] = i;
      c++;
    }
  }
  kar = NewCMPIArray(c, CMPI_string, NULL);

  for (i = 0; i < c; i++) {
    getPropertyQualsAt(cc, idx[i], &name, &quals, NULL, NULL);
    CMSetArrayElementAt(kar, i, &name, CMPI_string);
  }
  return kar;
}

static CMPIData
getMethod(CMPIConstClass * cc, const char *name, CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  CMPIData        rv_notfound = { 0, CMPI_notFound, {0} };
  CMPIData        rv;
  char           *mname;
  CMPICount       cnt = ClClassGetMethodCount(cls),
                  i;
  unsigned long   quals;

  for (i = 0; i < cnt; i++) {
    if (ClClassGetMethodAt(cls, i, &rv.type, &mname, &quals)) {
      if (rc)
        CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
      return rv_notfound;
    }
    if (strcasecmp(name, mname) == 0) {
      if (rc)
        CMSetStatus(rc, CMPI_RC_OK);
      return rv;
    };
  }
  if (rc)
    CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
  return rv_notfound;
}

static CMPIData
getMethodAt(CMPIConstClass * cc, CMPICount i, CMPIString **name,
               CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  char           *n;
  CMPIData        rv = { 0, CMPI_notFound, {0} };
  unsigned long   quals;
  if (ClClassGetMethodAt(cls, i, &rv.type, &n, &quals)) {
    if (rc)
      CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
    return rv;
  }
  if (name) {
    *name = sfcb_native_new_CMPIString(n, NULL, 0);
  }
  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);
  return rv;
}

static CMPICount
getMethodCount(CMPIConstClass * cc, CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);
  return (CMPICount) ClClassGetMethodCount(cls);
}

static CMPIData
getMethodQualifier(CMPIConstClass * cc, const char *meth, const char *cmq,
                 CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  ClSection      *mths = &cls->methods;
  CMPIData        rv_notFound = { 0, CMPI_notFound, {0} };
  CMPIData        rv;
  CMPICount       m = ClClassLocateMethod(&cls->hdr, mths, meth);
  CMPICount       num = ClClassGetMethQualifierCount(cls, m - 1);
  CMPICount       i;
  CMPIString     *name;

  for (i = 0; i < num; i++) {
    rv = internalGetMethQualAt(cc, m - 1, i, &name, rc);

    if (strcasecmp(cmq, (char*)name->hdl) == 0) {
      if (rc)
        CMSetStatus(rc, CMPI_RC_OK);
      return rv;
    }
  }
  if (rc)
    CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
  return rv_notFound;
}

static CMPIData
getMethodQualifierAt(CMPIConstClass * cc, const char *meth, CMPICount i,
                   CMPIString **name, CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  ClSection      *mths = &cls->methods;
  CMPICount       m = ClClassLocateMethod(&cls->hdr, mths, meth);
  return internalGetMethQualAt(cc, m - 1, i, name, rc);
}

static CMPICount
getMethodQualifierCount(CMPIConstClass * cc, const char *meth,
                      CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  ClSection      *mths = &cls->methods;
  CMPICount       m = ClClassLocateMethod(&cls->hdr, mths, meth);
  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);
  return ClClassGetMethQualifierCount(cls, m - 1);
}

static CMPIData
getMethodParameter(CMPIConstClass * cc, const char *meth, const char *cmq,
                 CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  ClSection      *mths = &cls->methods;
  CMPIData        rv_notFound = { 0, CMPI_notFound, {0} };
  CMPIData        rv;
  CMPICount       m = ClClassLocateMethod(&cls->hdr, mths, meth);
  CMPICount       num = ClClassGetMethParameterCount(cls, m - 1);
  CMPICount       i;
  CMPIString     *name;

  for (i = 0; i < num; i++) {
    rv = internalGetMethParamAt(cc, m - 1, i, &name, rc);

    if (strcasecmp(cmq, (char*)name->hdl) == 0) {
      if (rc)
        CMSetStatus(rc, CMPI_RC_OK);
      return rv;
    }
  }
  if (rc)
    CMSetStatus(rc, CMPI_RC_ERR_NOT_FOUND);
  return rv_notFound;
}

static CMPIData
getMethodParameterAt(CMPIConstClass * cc, const char *meth, CMPICount i,
                   CMPIString **name, CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  ClSection      *mths = &cls->methods;
  CMPICount       m = ClClassLocateMethod(&cls->hdr, mths, meth);
  return internalGetMethParamAt(cc, m - 1, i, name, rc);
}

static CMPICount
getMethodParameterCount(CMPIConstClass * cc, const char *meth,
                      CMPIStatus *rc)
{
  ClClass        *cls = (ClClass *) cc->hdl;
  ClSection      *mths = &cls->methods;
  CMPICount       m = ClClassLocateMethod(&cls->hdr, mths, meth);
  if (rc)
    CMSetStatus(rc, CMPI_RC_OK);
  return ClClassGetMethParameterCount(cls, m - 1);
}

struct _CMPIConstClass_FT ift = {
  1,
  release,
  cls_clone,
  getClassName,
  getProperty,
  getPropertyAt,
  getPropertyCount,
  getQualifier,
  getQualifierAt,
  getQualifierCount,
  getPropQualifier,
  getPropQualifierAt,
  getPropQualifierCount,
  getMethod,
  getMethodAt,
  getMethodCount,
  getMethodParameter,
  getMethodParameterAt,
  getMethodParameterCount,
  getMethodQualifier,
  getMethodQualifierAt,
  getMethodQualifierCount,
  getSuperClassName,
  getKeyList,
  toString,
  relocate,
  getCharClassName,
  getCharSuperClassName,
  isAssociation,
  isAbstract,
  isIndication
};

CMPIConstClass_FT *CMPIConstClassFT = &ift;

static CMPIConstClass *
cls_clone(CMPIConstClass * cc, CMPIStatus *rc)
{
  CMPIConstClass *cl = malloc(getConstClassSerializedSize(cc));
  cl->hdl = cl + 1;
  cl->ft = &ift;
  cl->refCount = 0;
  ClClassRebuildClass((ClClass *) cc->hdl, cl->hdl);
  if (rc)
    rc->rc = 0;
  return cl;
}

unsigned long
getConstClassSerializedSize(CMPIConstClass * cl)
{
  ClClass        *cli = (ClClass *) cl->hdl;
  return ClSizeClass(cli) + sizeof(CMPIConstClass);
}

void
getSerializedConstClass(CMPIConstClass * cl, void *area)
{
  memcpy(area, cl, sizeof(CMPIConstClass));
  ClClassRebuildClass((ClClass *) cl->hdl,
                      (void *) ((char *) area + sizeof(CMPIConstClass)));
}

CMPIConstClass *
relocateSerializedConstClass(void *area)
{
  CMPIConstClass *cl = (CMPIConstClass *) area;
  cl->hdl = cl + 1;
  cl->ft = &ift;
  cl->refCount = 1;             /* this is a kludge: disallow double free */
  ClClassRelocateClass((ClClass *) cl->hdl);
  return (CMPIConstClass *) cl;
}

MsgSegment
setConstClassMsgSegment(CMPIConstClass * cl)
{
  MsgSegment      s;
  s.data = cl;
  s.type = MSG_SEG_CONSTCLASS;
  s.length = getConstClassSerializedSize(cl);
  return s;
}

int
verifyPropertyList(CMPIConstClass * cl, char **list)
{
  CMPIStatus      rc;
  int             count = 0;
  for (; *list; list++) {
    getProperty(cl, *list, &rc);
    if (rc.rc == 0)
      count++;
  }
  return count;
}

CMPIConstClass
initConstClass(ClClass * cl)
{
  CMPIConstClass  c;
  c.hdl = cl;
  c.ft = &ift;
  c.refCount = 0;
  return c;
}
/* MODELINES */
/* DO NOT EDIT BELOW THIS COMMENT */
/* Modelines are added by 'make pretty' */
/* -*- Mode: C; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* vi:set ts=2 sts=2 sw=2 expandtab: */
