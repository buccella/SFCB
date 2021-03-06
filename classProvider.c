/*
 * classProvider.c
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
 * Class provider for sfcb .
 *
 */

#include "classProviderCommon.h"

#define LOCALCLASSNAME "ClassProvider"

typedef struct _Class_Register_FT Class_Register_FT;
struct _ClassRegister {
  void           *hdl;
  Class_Register_FT *ft;
  ClVersionRecord *vr;
  int             assocs,
                  topAssocs;
  char           *fn;
};
typedef struct _ClassRegister ClassRegister;

typedef struct _ClassBase {
  UtilHashTable  *ht;
  UtilHashTable  *it;
  MRWLOCK         mrwLock;
} ClassBase;

struct _Class_Register_FT {
  int             version;
  void            (*release) (ClassRegister * br);
  CMPIConstClass *(*getClass) (ClassRegister * br, const char *clsName);
  int             (*putClass) (ClassRegister * br, CMPIConstClass * cls);
  void            (*removeClass) (ClassRegister * br,
                                  const char *className);
  UtilList       *(*getChildren) (ClassRegister * br,
                                  const char *className);
  void            (*rLock) (ClassRegister * cr);
  void            (*wLock) (ClassRegister * cr);
  void            (*rUnLock) (ClassRegister * cr);
  void            (*wUnLock) (ClassRegister * cr);
};

extern Class_Register_FT *ClassRegisterFT;

static CMPIConstClass *getClass(ClassRegister * cr, const char *clsName);
int             traverseChildren(ClassRegister * cReg, const char *parent,
                                 const char *child);

static int      nsBaseLen;

static void
buildInheritanceTable(ClassRegister * cr)
{
  ClassBase      *cb = (ClassBase *) (cr + 1);
  UtilHashTable  *ct = cb->ht,
      *it;
  HashTableIterator *i;
  char           *cn;
  CMPIConstClass *cc;
  UtilList       *ul;

  it = cb->it = UtilFactory->newHashTable(61,
                                          UtilHashTable_charKey |
                                          UtilHashTable_ignoreKeyCase);

  for (i = ct->ft->getFirst(ct, (void **) &cn, (void **) &cc); i;
       i = ct->ft->getNext(ct, i, (void **) &cn, (void **) &cc)) {
    const char     *p = cc->ft->getCharSuperClassName(cc);
    if (p == NULL)
      continue;
    ul = it->ft->get(it, p);
    if (ul == NULL) {
      ul = UtilFactory->newList(memAddUtilList, memUnlinkEncObj);
      it->ft->put(it, p, ul);
    }
    ul->ft->prepend(ul, cc->ft->getCharClassName(cc));
  }
}

static void
release(ClassRegister * cr)
{
  ClassBase      *cb = (ClassBase *) cr->hdl;
  free(cr->fn);
  cb->ht->ft->release(cb->ht);
  free(cr);
}

void
rLock(ClassRegister * cr)
{
  ClassBase      *cb = (ClassBase *) cr->hdl;
  MReadLock(&cb->mrwLock);
}

void
wLock(ClassRegister * cr)
{
  ClassBase      *cb = (ClassBase *) cr->hdl;
  MWriteLock(&cb->mrwLock);
}

void
rUnLock(ClassRegister * cr)
{
  ClassBase      *cb = (ClassBase *) cr->hdl;
  MReadUnlock(&cb->mrwLock);
}

void
wUnLock(ClassRegister * cr)
{
  ClassBase      *cb = (ClassBase *) cr->hdl;
  MWriteUnlock(&cb->mrwLock);
}

static UtilList *
getChildren(ClassRegister * cr, const char *className)
{
  ClassBase      *cb = (ClassBase *) (cr + 1);
  return cb->it->ft->get(cb->it, className);
}

static void
removeChild(ClassRegister * cr, const char *pn, const char *chd)
{
  ClassBase      *cb = (ClassBase *) (cr + 1);
  char           *child;
  UtilList       *ul = cb->it->ft->get(cb->it, pn);

  if (ul)
    for (child = (char *) ul->ft->getFirst(ul); child;
         child = (char *) ul->ft->getNext(ul)) {
      if (strcasecmp(child, chd) == 0) {
        ul->ft->removeCurrent(ul);
        break;
      }
    }
}

static ClassRegister *
newClassRegister(char *fname)
{
  ClassRegister  *cr = malloc(sizeof(*cr) + sizeof(ClassBase));
  ClassBase      *cb = (ClassBase *) (cr + 1);
  FILE           *in;
  char            fin[1024];
  long            s,
                  total = 0;
  ClObjectHdr     hdr;
  ClVersionRecord *vrp = (ClVersionRecord *) & hdr;
  int             vRec = 0,
      first = 1;

  cr->hdl = cb;
  cr->ft = ClassRegisterFT;
  cr->vr = NULL;
  cr->assocs = cr->topAssocs = 0;

  strcpy(fin, fname);

  strcat(fin, "/classSchemas");
  in = fopen(fin, "r");

  if (in == NULL) {
    cb->ht = UtilFactory->newHashTable(61,
                                       UtilHashTable_charKey |
                                       UtilHashTable_ignoreKeyCase);
    cb->it =
        UtilFactory->newHashTable(61,
                                  UtilHashTable_charKey |
                                  UtilHashTable_ignoreKeyCase);
    MRWInit(&cb->mrwLock);
    return cr;
  }

  cr->fn = strdup(fin);
  cr->vr = NULL;
  cb->ht = UtilFactory->newHashTable(61,
                                     UtilHashTable_charKey |
                                     UtilHashTable_ignoreKeyCase);
  MRWInit(&cb->mrwLock);

  while ((s = fread(&hdr, 1, sizeof(hdr), in)) == sizeof(hdr)) {
    CMPIConstClass *cc = NULL;
    char           *buf = NULL;
    char           *cn;

    if (first) {
      if (vrp->size == sizeof(ClVersionRecord) && vrp->type == HDR_Version)
        vRec = 1;
      else if (vrp->size == sizeof(ClVersionRecord) << 24
               && vrp->type == HDR_Version) {
        mlogf(M_ERROR, M_SHOW,
              "--- %s is in wrong endian format - directory skipped\n",
              fin);
        fclose(in);
        return NULL;
      }
    }

    if (vRec == 0 && hdr.type != HDR_Class) {
      mlogf(M_ERROR, M_SHOW,
            "--- %s contains non-class record(s) - directory skipped\n",
            fin);
      fclose(in);
      return NULL;
    }

    buf = malloc(hdr.size);
    if (buf == NULL) {
      mlogf(M_ERROR, M_SHOW,
            "--- %s contains record(s) that are too long - directory skipped\n",
            fin);
      fclose(in);
      return NULL;
    }

    s = hdr.size;
    *((ClObjectHdr *) buf) = hdr;

    if (fread(buf + sizeof(hdr), 1, hdr.size - sizeof(hdr), in) ==
        hdr.size - sizeof(hdr)) {
      if (vRec) {
        cr->vr = (ClVersionRecord *) buf;
        if (strcmp(cr->vr->id, "sfcb-rep")) {
          mlogf(M_ERROR, M_SHOW,
                "--- %s contains invalid version record - directory skipped\n",
                fin);
          fclose(in);
          free(cr);
          free(buf);
          return NULL;
        }
        vRec = 0;
      }

      if (first) {
        int             v = -1;
        first = 0;
        if (ClVerifyObjImplLevel(cr->vr))
          continue;
        if (cr->vr)
          v = cr->vr->objImplLevel;
        mlogf(M_ERROR, M_SHOW,
              "--- %s contains unsupported object implementation format (%d) - directory skipped\n",
              fin, v);
        fclose(in);
        free(cr);
        free(buf);
        return NULL;
      }

      cc = NEW(CMPIConstClass);
      cc->hdl = buf;
      cc->ft = CMPIConstClassFT;
      cc->ft->relocate(cc);
      cn = (char *) cc->ft->getCharClassName(cc);

      if (strncmp(cn, "DMY_", 4) != 0) {
        total += s;
        cb->ht->ft->put(cb->ht, cn, cc);
        if (cc->ft->isAssociation(cc)) {
          cr->assocs++;
          if (cc->ft->getCharSuperClassName(cc) == NULL)
            cr->topAssocs++;
        }
      } else {
        free(cc->hdl);
        free(cc);
      }
    } else {
      mlogf(M_ERROR, M_SHOW,
            "--- %s contains invalid record(s) - directory skipped\n",
            fin);
      fclose(in);
      free(cr);
      free(buf);
      return NULL;
    }
    first = 0;

  }

  fclose(in);

  if (cr->vr) {
    mlogf(M_INFO, M_SHOW,
          "--- ClassProvider for %s (%d.%d-%d) using %ld bytes\n", fname,
          cr->vr->version, cr->vr->level, cr->vr->objImplLevel, total);
  } else
    mlogf(M_INFO, M_SHOW,
          "--- ClassProvider for %s (no-version) using %ld bytes\n", fname,
          total);

  buildInheritanceTable(cr);

  return cr;
}

static int
cpyClass(ClClass * cl, const CMPIConstClass * cc)
{
  ClClass        *ccl = (ClClass *) cc->hdl;
  CMPIData        d;
  CMPIParameter   p;
  CMPIType        t;
  char           *name;
  char           *refName = NULL;
  int             i,
                  m,
                  iq,
                  mq,
                  ip,
                  mp,
                  propId,
                  methId,
                  parmId;
  unsigned long   quals;
  ClProperty     *prop;
  ClMethod       *meth,
                 *newMeth;
  ClParameter    *parm,
                 *newParm;

  cl->quals |= ccl->quals;
  for (i = 0, m = ClClassGetQualifierCount(ccl); i < m; i++) {
    ClClassGetQualifierAt(ccl, i, &d, &name);
    ClClassAddQualifierSpecial(&cl->hdr, &cl->qualifiers, name, d,
                               &ccl->hdr);
  }

  for (i = 0, m = ClClassGetPropertyCount(ccl); i < m; i++) {
    ClClassGetPropertyAt(ccl, i, &d, &name, &quals, &refName);

    propId = ClClassAddProperty(cl, name, d, refName);
    if (refName) {
      free(refName);
    }
    prop =
        ((ClProperty *) ClObjectGetClSection(&cl->hdr, &cl->properties)) +
        propId - 1;

    for (iq = 0, mq = ClClassGetPropQualifierCount(ccl, i); iq < mq; iq++) {
      ClClassGetPropQualifierAt(ccl, i, iq, &d, &name);
      ClClassAddPropertyQualifierSpecial(&cl->hdr, prop, name, d,
                                         &ccl->hdr);
    }
  }

  for (i = 0, m = ClClassGetMethodCount(ccl); i < m; i++) {
    ClClassGetMethodAt(ccl, i, &t, &name, &quals);
    methId = ClClassAddMethod(cl, name, t);
    meth =
        ((ClMethod *) ClObjectGetClSection(&ccl->hdr, &ccl->methods)) +
        methId - 1;
    newMeth =
        ((ClMethod *) ClObjectGetClSection(&cl->hdr, &cl->methods)) +
        methId - 1;

    for (iq = 0, mq = ClClassGetMethQualifierCount(ccl, methId - 1);
         iq < mq; iq++) {
      ClClassGetMethQualifierAt(ccl, meth, iq, &d, &name);
      ClClassAddMethodQualifier(&cl->hdr, newMeth, name, d);
    }

    for (ip = 0, mp = ClClassGetMethParameterCount(ccl, methId - 1);
         ip < mp; ip++) {
      ClClassGetMethParameterAt(ccl, meth, ip, &p, &name);
      parmId = ClClassAddMethParameter(&cl->hdr, newMeth, name, p);
      parm = ((ClParameter *)
              ClObjectGetClSection(&ccl->hdr,
                                   &meth->parameters)) + parmId - 1;
      newParm = ((ClParameter *)
                 ClObjectGetClSection(&cl->hdr,
                                      &newMeth->parameters)) + parmId - 1;

      for (iq = 0, mq = ClClassGetMethParamQualifierCount(parm);
           iq < mq; iq++) {
        ClClassGetMethParamQualifierAt(ccl, parm, iq, &d, &name);
        ClClassAddMethParamQualifier(&cl->hdr, newParm, name, d);
      }
    }
  }
  return 0;
}

static CMPIStatus
mergeParents(ClassRegister * cr, ClClass * cl, char *p,
             const CMPIConstClass * cc)
{
  CMPIStatus      st = { CMPI_RC_OK, NULL };
  CMPIConstClass *pcc = NULL;

  if (p) {
    pcc = getClass(cr, p);
    if (pcc == NULL) {
      st.rc = CMPI_RC_ERR_INVALID_SUPERCLASS;
      return st;
    }
    cpyClass(cl, pcc);
  }

  if (cc) {
    cpyClass(cl, cc);
  }

  return st;
}

static CMPIStatus
addClass(ClassRegister * cr, CMPIConstClass * ccp, char *cn, char *p)
{
  CMPIStatus      st = { CMPI_RC_OK, NULL };
  ClassBase      *cb = (ClassBase *) (cr + 1);
  UtilHashTable  *it = cb->it;
  UtilList       *ul;
  char           *pn = p;
  CMPIConstClass *cc = ccp;
  ClClass        *mc;
  FILE           *rep;

  if (p) {
    mc = ClClassNew(cn, p);
    st = mergeParents(cr, mc, pn, ccp);
    if (st.rc != CMPI_RC_OK) {
      ClClassFreeClass(mc);
      return st;
    }
    ccp->hdl = mc;
  }
  cc = ccp->ft->clone(ccp, NULL);
  mc = (ClClass *) cc->hdl;

  cb->ht->ft->put(cb->ht, strdup(cn), cc);
  rep = fopen(cr->fn, "a");
  fwrite(mc, 1, mc->hdr.size, rep);
  fclose(rep);

  if (cc->ft->isAssociation(cc)) {
    cr->assocs++;
    if (p == NULL)
      cr->topAssocs++;
  }

  if (p) {
    ul = it->ft->get(it, p);
    if (ul == NULL) {
      ul = UtilFactory->newList(memAddUtilList, memUnlinkEncObj);
      it->ft->put(it, p, ul);
    }
    ul->ft->prepend(ul, cn);
  }

  return st;
}

static UtilHashTable *
gatherNameSpaces(char *dn, UtilHashTable * ns, int first)
{
  DIR            *dir,
                 *dir_test;
  struct dirent  *de;
  char           *n = NULL;
  int             l;
  ClassRegister  *cr;

  if (ns == NULL) {
    ns = UtilFactory->newHashTable(61,
                                   UtilHashTable_charKey |
                                   UtilHashTable_ignoreKeyCase);
    nsBaseLen = strlen(dn) + 1;
  }

  dir = opendir(dn);
  if (dir) {
    while ((de = readdir(dir)) != NULL) {
      if (strcmp(de->d_name, ".") == 0)
        continue;
      if (strcmp(de->d_name, "..") == 0)
        continue;
      l = strlen(dn) + strlen(de->d_name) + 4;
      n = malloc(l + 8);
      strcpy(n, dn);
      strcat(n, "/");
      strcat(n, de->d_name);
      dir_test = opendir(n);
      if (dir_test == NULL) {
        free(n);
        continue;
      }
      closedir(dir_test);
      cr = newClassRegister(n);
      if (cr) {
        ns->ft->put(ns, strdup(n + nsBaseLen), cr);
        gatherNameSpaces(n, ns, 0);
      }
      free(n);
    }
    closedir(dir);
  } else if (first || dir == NULL) {
    mlogf(M_ERROR, M_SHOW, "--- Repository %s not found\n", dn);
  }
  return ns;
}

static UtilHashTable *
buildClassRegisters()
{
  char           *dir;
  char           *dn;

  setupControl(configfile);

  if (getControlChars("registrationDir", &dir)) {
    dir = "/var/lib/sfcb/registration";
  }

  dn = alloca(strlen(dir) + 32);
  strcpy(dn, dir);
  if (dir[strlen(dir) - 1] != '/')
    strcat(dn, "/");
  strcat(dn, "repository");
  return gatherNameSpaces(dn, NULL, 1);
}

static void
nsHt_init()
{
  nsHt = buildClassRegisters();
}

static ClassRegister *
getNsReg(const CMPIObjectPath * ref, int *rc)
{
  char           *ns;
  CMPIString     *nsi = CMGetNameSpace(ref, NULL);
  ClassRegister  *cReg;
  *rc = 0;

  pthread_once(&nsHt_once, nsHt_init);

  if (nsHt == NULL) {
    mlogf(M_ERROR, M_SHOW,
          "--- ClassProvider: namespace hash table not initialized\n");
    *rc = 1;
    return NULL;
  }

  if (nsi && nsi->hdl) {
    ns = (char *) nsi->hdl;
    cReg = nsHt->ft->get(nsHt, ns);
    return cReg;
  }

  *rc = 1;
  return NULL;
}

static int
putClass(ClassRegister * cr, CMPIConstClass * cls)
{
  ClassBase      *cb = (ClassBase *) cr->hdl;
  return cb->ht->ft->put(cb->ht, cls->ft->getCharClassName(cls), cls);
}

static void
removeClass(ClassRegister * cr, const char *clsName)
{
  FILE           *repold,
                 *repnew;
  char           *tmpfn;
  int             s;
  ClObjectHdr     hdr;
  ClassBase      *cb = (ClassBase *) cr->hdl;

  cb->ht->ft->remove(cb->ht, clsName);

  repold = fopen(cr->fn, "r");
  tmpfn = malloc(strlen(cr->fn) + 8);
  strcpy(tmpfn, cr->fn);
  strcat(tmpfn, ".tmp");
  repnew = fopen(tmpfn, "w");

  while ((s = fread(&hdr, 1, sizeof(hdr), repold)) == sizeof(hdr)) {
    CMPIConstClass  cc;
    char           *buf = NULL;
    char           *cn;

    buf = malloc(hdr.size);
    *((ClObjectHdr *) buf) = hdr;

    if (fread(buf + sizeof(hdr), 1, hdr.size - sizeof(hdr), repold) ==
        hdr.size - sizeof(hdr)) {
      if (hdr.type == HDR_Class) {
        cc.hdl = buf;
        cc.ft = CMPIConstClassFT;
        cc.ft->relocate(&cc);
        cn = (char *) cc.ft->getCharClassName(&cc);
        if (strcasecmp(clsName, cn) == 0) {
          free(buf);
          continue;
        }
      }
      fwrite(buf, 1, hdr.size, repnew);
    }
    free(buf);
  }
  fclose(repold);
  fclose(repnew);

  unlink(cr->fn);
  rename(tmpfn, cr->fn);

  free(tmpfn);
}

static CMPIConstClass *
getClass(ClassRegister * cr, const char *clsName)
{
  _SFCB_ENTER(TRACE_PROVIDERS, "getClass");
  _SFCB_TRACE(1, ("--- classname %s cReg %p", clsName, cr));
  ClassBase      *cb = (ClassBase *) cr->hdl;
  CMPIConstClass *cls = cb->ht->ft->get(cb->ht, clsName);
  _SFCB_RETURN(cls);
}

static Class_Register_FT ift = {
  1,
  release,
  getClass,
  putClass,
  removeClass,
  getChildren,
  rLock,
  wLock,
  rUnLock,
  wUnLock
};

Class_Register_FT *ClassRegisterFT = &ift;

/*
 * ------------------------------------------------------------------ *
 * Class MI Cleanup
 * ------------------------------------------------------------------ 
 */

static CMPIStatus
ClassProviderCleanup(CMPIClassMI * mi, const CMPIContext *ctx)
{
  HashTableIterator *i,
                 *ii;
  ClassRegister  *cReg;
  ClassBase      *cb;
  UtilHashTable  *ct,
                 *it;
  CMPIConstClass *cc;
  UtilList       *ul;
  char           *cn;

  for (i = nsHt->ft->getFirst(nsHt, (void **) &cn, (void **) &cReg); i;
       i = nsHt->ft->getNext(nsHt, i, (void **) &cn, (void **) &cReg)) {
    cb = (ClassBase *) (cReg + 1);
    ct = cb->ht;
    for (ii = ct->ft->getFirst(ct, (void **) &cn, (void **) &cc); ii;
         ii = ct->ft->getNext(ct, ii, (void **) &cn, (void **) &cc)) {
      cc->ft->release(cc);
    }
    ct->ft->release(ct);
    it = cb->it;
    for (ii = it->ft->getFirst(it, (void **) &cn, (void **) &ul); ii;
         ii = it->ft->getNext(it, ii, (void **) &cn, (void **) &ul)) {
      ul->ft->release(ul);
    }
    it->ft->release(it);
  }
  nsHt->ft->release(nsHt);

  CMReturn(CMPI_RC_OK);
}

/*
 * ------------------------------------------------------------------ *
 * Class MI Functions
 * ------------------------------------------------------------------ 
 */

static void
loopOnChildNames(ClassRegister * cReg, char *cn, const CMPIResult *rslt)
{
  CMPIObjectPath *op;
  UtilList       *ul = getChildren(cReg, cn);
  char           *child;
  if (ul)
    for (child = (char *) ul->ft->getFirst(ul); child;
         child = (char *) ul->ft->getNext(ul)) {
      op = CMNewObjectPath(_broker, NULL, child, NULL);
      CMReturnObjectPath(rslt, op);
      loopOnChildNames(cReg, child, rslt);
    }
}

static CMPIStatus
ClassProviderEnumClassNames(CMPIClassMI * mi,
                            const CMPIContext *ctx,
                            const CMPIResult *rslt, const CMPIObjectPath * ref)
{
  CMPIStatus      st = { CMPI_RC_OK, NULL };
  char           *cn = NULL;
  CMPIFlags       flgs = 0;
  CMPIString     *cni;
  ClassBase      *cb;
  HashTableIterator *it;
  char           *key;
  int             rc;
  CMPIConstClass *cls;
  CMPIObjectPath *op;
  ClassRegister  *cReg;
  char           *ns;

  _SFCB_ENTER(TRACE_PROVIDERS, "ClassProviderEnumClassNames");

  cReg = getNsReg(ref, &rc);
  if (cReg == NULL) {
    CMPIStatus      st = { CMPI_RC_ERR_INVALID_NAMESPACE, NULL };
    _SFCB_RETURN(st);
  }

  ns = (char *) CMGetNameSpace(ref, NULL)->hdl;
  flgs = ctx->ft->getEntry(ctx, CMPIInvocationFlags, NULL).value.uint32;
  cni = ref->ft->getClassName(ref, NULL);
  if (cni) {
    cn = (char *) cni->hdl;
    if (cn && *cn == 0)
      cn = NULL;
  }
  cb = (ClassBase *) cReg->hdl;

  cReg->ft->rLock(cReg);

  if (cn && strcasecmp(cn, "$ClassProvider$") == 0)
    cn = NULL;

  if (cn == NULL) {
    for (it = cb->ht->ft->getFirst(cb->ht, (void **) &key, (void **) &cls);
         key && it && cls;
         it =
         cb->ht->ft->getNext(cb->ht, it, (void **) &key, (void **) &cls)) {
      if ((flgs & CMPI_FLAG_DeepInheritance)
          || cls->ft->getCharSuperClassName(cls) == NULL) {
        if (((flgs & FL_assocsOnly) == 0) || cls->ft->isAssociation(cls)) {
          op = CMNewObjectPath(_broker, ns, key, NULL);
          CMReturnObjectPath(rslt, op);
        }
      }
    }
  } else {
    cls = getClass(cReg, cn);
    if (cls == NULL) {
      st.rc = CMPI_RC_ERR_INVALID_CLASS;
    } else if ((flgs & CMPI_FLAG_DeepInheritance) == 0) {
      UtilList       *ul = getChildren(cReg, cn);
      char           *child;
      if (ul)
        for (child = (char *) ul->ft->getFirst(ul); child;
             child = (char *) ul->ft->getNext(ul)) {
          op = CMNewObjectPath(_broker, ns, child, NULL);
          CMReturnObjectPath(rslt, op);
        }
    } else if (flgs & CMPI_FLAG_DeepInheritance) {
      if (((flgs & FL_assocsOnly) == 0) || cls->ft->isAssociation(cls))
        loopOnChildNames(cReg, cn, rslt);
    }
  }

  cReg->ft->rUnLock(cReg);

  _SFCB_RETURN(st);
}

static void
loopOnChildren(ClassRegister * cReg, char *cn, const CMPIResult *rslt)
{
  UtilList       *ul = getChildren(cReg, cn);
  char           *child;
  if (ul)
    for (child = (char *) ul->ft->getFirst(ul); child;
         child = (char *) ul->ft->getNext(ul)) {
      CMPIConstClass *cl = getClass(cReg, child);
      CMReturnInstance(rslt, (CMPIInstance *) cl);
      loopOnChildren(cReg, child, rslt);
    }
}

static CMPIStatus
ClassProviderEnumClasses(CMPIClassMI * mi,
                         const CMPIContext *ctx,
                         const CMPIResult *rslt, const CMPIObjectPath * ref)
{
  CMPIStatus      st = { CMPI_RC_OK, NULL };
  char           *cn = NULL;
  CMPIFlags       flgs = 0;
  CMPIString     *cni;
  ClassBase      *cb;
  HashTableIterator *it;
  char           *key;
  int             rc;
  CMPIConstClass *cls;
  ClassRegister  *cReg;

  _SFCB_ENTER(TRACE_PROVIDERS, "ClassProviderEnumClasss");

  cReg = getNsReg(ref, &rc);
  if (cReg == NULL) {
    CMPIStatus      st = { CMPI_RC_ERR_INVALID_NAMESPACE, NULL };
    _SFCB_RETURN(st);
  }

  cReg->ft->rLock(cReg);

  flgs = ctx->ft->getEntry(ctx, CMPIInvocationFlags, NULL).value.uint32;
  cni = ref->ft->getClassName(ref, NULL);
  if (cni) {
    cn = (char *) cni->hdl;
    if (cn && *cn == 0)
      cn = NULL;
  }
  cb = (ClassBase *) cReg->hdl;

  if (cn == NULL) {
    for (it = cb->ht->ft->getFirst(cb->ht, (void **) &key, (void **) &cls);
         key && it && cls;
         it =
         cb->ht->ft->getNext(cb->ht, it, (void **) &key, (void **) &cls)) {
      if ((flgs & CMPI_FLAG_DeepInheritance)
          || cls->ft->getCharSuperClassName(cls) == NULL) {
        CMReturnInstance(rslt, (CMPIInstance *) cls);
      }
    }
  } else {
    cls = getClass(cReg, cn);
    if (cls == NULL) {
      st.rc = CMPI_RC_ERR_INVALID_CLASS;
    } else if ((flgs & CMPI_FLAG_DeepInheritance) == 0) {
      UtilList       *ul = getChildren(cReg, cn);
      char           *child;
      if (ul)
        for (child = (char *) ul->ft->getFirst(ul); child;
             child = (char *) ul->ft->getNext(ul)) {
          cls = getClass(cReg, child);
          CMReturnInstance(rslt, (CMPIInstance *) cls);
        }
    } else if (flgs & CMPI_FLAG_DeepInheritance) {
      loopOnChildren(cReg, cn, rslt);
    }
  }

  cReg->ft->rUnLock(cReg);

  _SFCB_RETURN(st);
}

static CMPIStatus
ClassProviderGetClass(CMPIClassMI * mi,
                      const CMPIContext *ctx,
                      const CMPIResult *rslt,
                      const CMPIObjectPath * ref, const char **properties)
{
  CMPIStatus      st = { CMPI_RC_OK, NULL };
  CMPIString     *cn = CMGetClassName(ref, NULL);
  CMPIConstClass *cl;
  ClassRegister  *cReg;
  int             rc;

  _SFCB_ENTER(TRACE_PROVIDERS, "ClassProviderGetClass");
  _SFCB_TRACE(1, ("--- ClassName=\"%s\"", (char *) cn->hdl));

  cReg = getNsReg(ref, &rc);
  if (cReg == NULL) {
    CMPIStatus      st = { CMPI_RC_ERR_INVALID_NAMESPACE, NULL };
    _SFCB_RETURN(st);
  }

  cReg->ft->rLock(cReg);

  cl = getClass(cReg, (char *) cn->hdl);
  if (cl) {
    _SFCB_TRACE(1, ("--- Class found"));
    if(properties) {
      filterClass(cl, properties);
    }
    CMReturnInstance(rslt, (CMPIInstance *) cl);
  } else {
    _SFCB_TRACE(1, ("--- Class not found"));
    st.rc = CMPI_RC_ERR_NOT_FOUND;
  }

  cReg->ft->rUnLock(cReg);

  _SFCB_RETURN(st);
}

static CMPIStatus
ClassProviderCreateClass(CMPIClassMI * mi,
                         const CMPIContext *ctx,
                         const CMPIResult *rslt,
                         const CMPIObjectPath * ref, const CMPIConstClass * cc)
{
  ClassRegister  *cReg;
  int             rc;

  CMPIStatus      st = { CMPI_RC_OK, NULL };

  _SFCB_ENTER(TRACE_PROVIDERS, "ClassProviderCreateClass");

  cReg = getNsReg(ref, &rc);
  if (cReg == NULL) {
    CMPIStatus      st = { CMPI_RC_ERR_INVALID_NAMESPACE, NULL };
    _SFCB_RETURN(st);
  }

  char           *pn = (char *) cc->ft->getCharSuperClassName(cc);
  char           *cn = (char *) cc->ft->getCharClassName(cc);

  if (getClass(cReg, cn)) {
    st.rc = CMPI_RC_ERR_ALREADY_EXISTS;
    _SFCB_RETURN(st);
  }
  if (pn && getClass(cReg, pn) == NULL) {
    st.rc = CMPI_RC_ERR_INVALID_SUPERCLASS;
    _SFCB_RETURN(st);
  }

  cReg->ft->wLock(cReg);

  st = addClass(cReg, (CMPIConstClass*)cc, cn, pn);

  cReg->ft->wUnLock(cReg);

  _SFCB_RETURN(st);
}

static CMPIStatus
ClassProviderSetClass(CMPIClassMI * mi,
                      const CMPIContext *ctx,
                      const CMPIResult *rslt,
                      const CMPIObjectPath * cop, const CMPIConstClass * ci)
{
  return notSupSt;
}

static CMPIStatus
ClassProviderDeleteClass(CMPIClassMI * mi,
                         const CMPIContext *ctx,
                         const CMPIResult *rslt, const CMPIObjectPath * cop)
{
  ClassRegister  *cReg;
  CMPIConstClass *cl;
  int             rc;

  CMPIStatus      st = { CMPI_RC_OK, NULL };

  _SFCB_ENTER(TRACE_PROVIDERS, "ClassProviderDeleteClass");

  cReg = getNsReg(cop, &rc);
  if (cReg == NULL) {
    CMPIStatus      st = { CMPI_RC_ERR_INVALID_NAMESPACE, NULL };
    _SFCB_RETURN(st);
  }

  char           *cn = (char *) cop->ft->getClassName(cop, NULL)->hdl;

  cl = getClass(cReg, cn);
  if (cl == NULL) {
    st.rc = CMPI_RC_ERR_NOT_FOUND;
    _SFCB_RETURN(st);
  }

  UtilList       *ul = getChildren(cReg, cn);
  if (ul && ul->ft->size(ul)) {
    // char *child;
    // for (child =(char*)ul->ft->getFirst(ul); child;
    // child=(char*)ul->ft->getNext(ul)) 
    // printf("child: %s\n",child);
    st.rc = CMPI_RC_ERR_CLASS_HAS_CHILDREN;
    _SFCB_RETURN(st);
  }

  char           *pn = (char *) cl->ft->getCharSuperClassName(cl);

  cReg->ft->wLock(cReg);

  if (pn)
    removeChild(cReg, pn, cn);
  removeClass(cReg, cn);

  cReg->ft->wUnLock(cReg);

  _SFCB_RETURN(st);
}

/*
 * ---------------------------------------------------------------------------
 */
/*
 * Method Provider Interface 
 */
/*
 * ---------------------------------------------------------------------------
 */

extern CMPIBoolean isAbstract(CMPIConstClass * cc);

static int
repCandidate(ClassRegister * cReg, char *cn)
{
  CMPIConstClass *cl = getClass(cReg, cn);
  if (isAbstract(cl))
    return 0;
  ProviderInfo   *info;

  _SFCB_ENTER(TRACE_PROVIDERS, "repCandidate");

  if (strcasecmp(cn, "cim_indicationfilter") == 0 ||
      strcasecmp(cn, "cim_indicationsubscription") == 0)
    _SFCB_RETURN(0);

  while (cn != NULL) {
    info = pReg->ft->getProvider(pReg, cn, INSTANCE_PROVIDER);
    if (info)
      _SFCB_RETURN(0);
    cn = (char *) cl->ft->getCharSuperClassName(cl);
    if (cn == NULL)
      break;
    cl = getClass(cReg, cn);
  }
  _SFCB_RETURN(1);
}

static void
loopOnChildChars(ClassRegister * cReg, char *cn, CMPIArray *ar, int *i,
                 int ignprov)
{
  UtilList       *ul = getChildren(cReg, cn);
  char           *child;

  _SFCB_ENTER(TRACE_PROVIDERS, "loopOnChildChars");
  _SFCB_TRACE(1, ("--- class %s", cn));

  if (ul)
    for (child = (char *) ul->ft->getFirst(ul); child;
         child = (char *) ul->ft->getNext(ul)) {
      if (ignprov || repCandidate(cReg, child)) {
        CMSetArrayElementAt(ar, *i, child, CMPI_chars);
        *i = (*i) + 1;
      }
      loopOnChildChars(cReg, child, ar, i, ignprov);
    }
  _SFCB_EXIT();
}

static void
loopOnChildCount(ClassRegister * cReg, char *cn, int *i, int ignprov)
{
  UtilList       *ul = getChildren(cReg, cn);
  char           *child;

  _SFCB_ENTER(TRACE_PROVIDERS, "loopOnChildCount");

  if (ul)
    for (child = (char *) ul->ft->getFirst(ul); child;
         child = (char *) ul->ft->getNext(ul)) {
      if (ignprov || repCandidate(cReg, child))
        *i = (*i) + 1;
      loopOnChildCount(cReg, child, i, ignprov);
    }
  _SFCB_EXIT();
}

/* ClassProviderMethodCleanup */
static CMPIStatus okCleanup(ClassProvider,Method);

static CMPIStatus
ClassProviderInvokeMethod(CMPIMethodMI * mi,
                          const CMPIContext *ctx,
                          const CMPIResult *rslt,
                          const CMPIObjectPath * ref,
                          const char *methodName,
                          const CMPIArgs * in, CMPIArgs * out)
{
  CMPIStatus      st = { CMPI_RC_OK, NULL };
  CMPIArray      *ar;
  int             rc;
  ClassRegister  *cReg;

  _SFCB_ENTER(TRACE_PROVIDERS, "ClassProviderInvokeMethod");

  cReg = getNsReg(ref, &rc);
  if (cReg == NULL) {
    CMPIStatus      st = { CMPI_RC_ERR_INVALID_NAMESPACE, NULL };
    _SFCB_RETURN(st);
  }

  if (strcasecmp(methodName, "getchildren") == 0) {
    CMPIData        cn = CMGetArg(in, "class", NULL);
    _SFCB_TRACE(1, ("--- getchildren %s", (char *) cn.value.string->hdl));

    cReg->ft->rLock(cReg);

    if (cn.type == CMPI_string && cn.value.string && cn.value.string->hdl) {
      char           *child;
      int             l = 0,
          i = 0;
      UtilList       *ul =
          getChildren(cReg, (char *) cn.value.string->hdl);
      if (ul)
        l = ul->ft->size(ul);
      ar = CMNewArray(_broker, l, CMPI_string, NULL);
      if (ul)
        for (child = (char *) ul->ft->getFirst(ul); child; child = (char *)
             ul->ft->getNext(ul)) {
          CMSetArrayElementAt(ar, i++, child, CMPI_chars);
        }
      st = CMAddArg(out, "children", &ar, CMPI_stringA);
    } else {
    }

    cReg->ft->rUnLock(cReg);

  }

  else if (strcasecmp(methodName, "getallchildren") == 0) {
    int             ignprov = 0;
    CMPIStatus      st;
    CMPIData        cn = CMGetArg(in, "class", &st);

    cReg->ft->rLock(cReg);

    if (st.rc != CMPI_RC_OK) {
      cn = CMGetArg(in, "classignoreprov", NULL);
      ignprov = 1;
    }
    _SFCB_TRACE(1,
                ("--- getallchildren %s", (char *) cn.value.string->hdl));
    if (cn.type == CMPI_string && cn.value.string && cn.value.string->hdl) {
      int             n = 0,
          i = 0;
      loopOnChildCount(cReg, (char *) cn.value.string->hdl, &n, ignprov);
      _SFCB_TRACE(1, ("--- count %d", n));
      ar = CMNewArray(_broker, n, CMPI_string, NULL);
      if (n) {
        _SFCB_TRACE(1, ("--- loop %s", (char *) cn.value.string->hdl));
        loopOnChildChars(cReg, (char *) cn.value.string->hdl, ar, &i,
                         ignprov);
      }
      st = CMAddArg(out, "children", &ar, CMPI_stringA);
    } else {
    }

    cReg->ft->rUnLock(cReg);
  }

  else if (strcasecmp(methodName, "getassocs") == 0) {
    ar = CMNewArray(_broker, cReg->topAssocs, CMPI_string, NULL);
    ClassBase      *cb = (ClassBase *) (cReg + 1);
    UtilHashTable  *ct = cb->ht;
    HashTableIterator *i;
    char           *cn;
    CMPIConstClass *cc;
    int             n;

    cReg->ft->rLock(cReg);

    for (n = 0, i = ct->ft->getFirst(ct, (void **) &cn, (void **) &cc); i;
         i = ct->ft->getNext(ct, i, (void **) &cn, (void **) &cc)) {
      if (cc->ft->isAssociation(cc)
          && cc->ft->getCharSuperClassName(cc) == NULL) {
        /*
         * add top-level association class 
         */
        CMSetArrayElementAt(ar, n++, cn, CMPI_chars);
      }
    }
    CMAddArg(out, "assocs", &ar, CMPI_stringA);

    cReg->ft->rUnLock(cReg);
  }

  else if (strcasecmp(methodName, "ischild") == 0) {
    char           *parent = (char *) CMGetClassName(ref, NULL)->hdl;
    char           *chldn =
        (char *) CMGetArg(in, "child", NULL).value.string->hdl;
    st.rc = traverseChildren(cReg, parent, chldn);
  }

  else if (strcasecmp(methodName, "listnamespaces") == 0) {

    HashTableIterator *hit;
    char           *key;
    ClassRegister  *cReg;

    CMPIArray* ar = CMNewArray(_broker, nsHt->ft->size(nsHt), CMPI_string, NULL);
    int i = 0;

    /* req for specific ns */
    CMPIData nsd = CMGetArg(in, "ns", &st);
    if (st.rc == CMPI_RC_OK) {
      char* ns = CMGetCharPtr(nsd.value.string);
        ClassRegister  *cReg = NULL;
        cReg = nsHt->ft->get(nsHt, ns);
        st.rc = (cReg) ? CMPI_RC_OK : CMPI_RC_ERR_NOT_FOUND;
    }
    else {
      for (hit = nsHt->ft->getFirst(nsHt, (void **) &key, (void **) &cReg);
           key && hit;
           hit =
             nsHt->ft->getNext(nsHt, hit, (void **) &key, (void **) &cReg)) {

        CMSetArrayElementAt(ar, i++, key, CMPI_chars);
      }
  
      CMAddArg(out, "nslist", &ar, CMPI_stringA);
      st.rc = CMPI_RC_OK;
    }

  }

  else if (strcasecmp(methodName, "_startup") == 0) {

    /* let providerMgr know that we're odne init'ing  */
    semRelease(sfcbSem,INIT_CLASS_PROV_ID);

    st.rc = CMPI_RC_OK;
  }

  else {
    mlogf(M_ERROR, M_SHOW,
          "--- ClassProvider: Invalid invokeMethod request %s\n",
          methodName);
    st.rc = CMPI_RC_ERR_METHOD_NOT_FOUND;
  }
  _SFCB_RETURN(st);
}

int
traverseChildren(ClassRegister * cReg, const char *parent,
                 const char *chldn)
{
  char           *child;
  int             rc = CMPI_RC_ERR_FAILED;
  UtilList       *ul = getChildren(cReg, parent);

  cReg->ft->rLock(cReg);

  if (ul)
    for (child = (char *) ul->ft->getFirst(ul); child;
         child = (char *) ul->ft->getNext(ul)) {
      if (strcasecmp(child, chldn) == 0) {
        rc = CMPI_RC_OK;
        break;
      } else {
        rc = traverseChildren(cReg, child, chldn);
        if (rc == CMPI_RC_OK)
          break;
      }
    }

  cReg->ft->rUnLock(cReg);
  return rc;
}

CMClassMIStub(ClassProvider, ClassProvider, _broker, CMNoHook);

CMMethodMIStub(ClassProvider, ClassProvider, _broker, CMNoHook);

// 
// 
/* MODELINES */
/* DO NOT EDIT BELOW THIS COMMENT */
/* Modelines are added by 'make pretty' */
/* -*- Mode: C; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* vi:set ts=2 sts=2 sw=2 expandtab: */
