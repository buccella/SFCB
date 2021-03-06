
/*
 * providerMgr.h
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
 * Based on concepts developed by Viktor Mihajlovski <mihajlov@de.ibm.com>
 *
 * Description:
 *
 * Provider manager support.
 *
 */

#ifndef providerMgr_h
#define providerMgr_h

#include "msgqueue.h"
#include "httpComm.h"
#include "cmpi/cmpidt.h"

#define OPS_GetClass 1
#define OPS_GetInstance 2
#define OPS_DeleteClass 3
#define OPS_DeleteInstance 4
#define OPS_CreateClass 5
#define OPS_CreateInstance 6
#define OPS_ModifyClass 7
#define OPS_ModifyInstance 8
#define OPS_EnumerateClasses 9
#define OPS_EnumerateClassNames 10
#define OPS_EnumerateInstances 11
#define OPS_EnumerateInstanceNames 12
#define OPS_ExecQuery 13
#define OPS_Associators 14
#define OPS_AssociatorNames 15
#define OPS_References 16
#define OPS_ReferenceNames 17
#define OPS_GetProperty 18
#define OPS_SetProperty 19
#define OPS_GetQualifier 20
#define OPS_SetQualifier 21
#define OPS_DeleteQualifier 22
#define OPS_EnumerateQualifiers 23
#define OPS_InvokeMethod 24

#define OPS_LoadProvider 25
#define OPS_PingProvider 26

#define OPS_IndicationLookup   27
#define OPS_ActivateFilter     28
#define OPS_DeactivateFilter   29
#define OPS_DisableIndications 30
#define OPS_EnableIndications  31

#define OPS_OpenEnumerateInstancePaths 32
#define OPS_OpenEnumerateInstances 33
#define OPS_OpenAssociatorInstancePaths 34
#define OPS_OpenAssociatorInstances 35
#define OPS_OpenReferenceInstancePaths 36
#define OPS_OpenReferenceInstances 37
#define OPS_OpenQueryInstances 38
#define OPS_PullInstances 39
#define OPS_PullInstancesWithPath 40
#define OPS_PullInstancePaths 41
#define OPS_CloseEnumeration 42
#define OPS_EnumerationCount 43

// this macro must be adjusted when when BinRequestHdr is changed
#define BINREQ(oper,count) {{oper,0,NULL,0,0,count}}

typedef struct operationHdr {
  unsigned short  type;
  unsigned short  options;
#define OH_Internal 2
  unsigned long   count;
  MsgSegment      nameSpace;
  MsgSegment      className;
  union {
    MsgSegment      resultClass;
    MsgSegment      query;
  };
  union {
    MsgSegment      role;
    MsgSegment      queryLang;
  };
  MsgSegment      assocClass;
  MsgSegment      resultRole;
} OperationHdr;

// macro BINREQ must be adjusted when when BinRequestHdr is changed
typedef struct binRequestHdr {
  unsigned short  operation;
  unsigned short  options;
#define BRH_NoResp 1
#define BRH_Internal 2
  void           *provId;
  unsigned int    sessionId;
  unsigned int    flags;
  unsigned long   count;        // maps to MsgList
  MsgSegment      object[0]; /* points to the start of alloc'd array of MsgSegments 
                                representing params for request ( */
} BinRequestHdr;

typedef struct binResponseHdr {
  long            rc;
  CMPIData        rv;           // need to check for string returns
  MsgSegment      rvEnc;
  unsigned char   rvValue,
                  chunkedMode,
                  moreChunks;
  unsigned long   count;        // number of MsgSegments in response
  MsgSegment      object[1]; /* WARNING: brokerUpc references segments positionally! */
} BinResponseHdr;

struct chunkFunctions;
struct commHndl;
struct requestHdr;

typedef union provIds {
  void           *ids;
  struct {
    SFCB_HALFWORD   procId;
    SFCB_HALFWORD   provId;
  };
} ProvIds;

typedef struct provAddr {
  int             socket;
  ProvIds         ids;
} ProvAddr;

typedef struct binRequestContext {
  OperationHdr   *oHdr;
  BinRequestHdr  *bHdr;
  struct requestHdr *rHdr;
  unsigned long   bHdrSize;
  int             requestor;
  int             chunkedMode,
                  xmlAs,
                  noResp;
  struct chunkFunctions *chunkFncs;
  struct commHndl *commHndl;
  CMPIType        type;
  ProvAddr        provA;
  ProvAddr       *pAs;
  char           *httpHost;
  unsigned long   pCount,
                  pDone;
  unsigned long   rCount;
  int             rc;
  MsgXctl        *ctlXdata;
} BinRequestContext;

#define XML_asObj 1
#define XML_asClassName 2
#define XML_asClass 4
#define XML_asObjectPath 8

typedef struct chunkFunctions {
  void            (*writeChunk) (BinRequestContext *, BinResponseHdr *);
} ChunkFunctions;

typedef struct getClassReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      objectPath;
  MsgSegment      userRole;
  MsgSegment      properties[1];
} GetClassReq;
#define GC_REQ_REG_SEGMENTS 3

typedef struct enumClassNamesReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      objectPath;
  MsgSegment      userRole;
} EnumClassNamesReq;
#define ECN_REQ_REG_SEGMENTS 3

typedef struct enumClassesReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      objectPath;
  MsgSegment      userRole;
} EnumClassesReq;
#define EC_REQ_REG_SEGMENTS 3

typedef struct enumInstanceNamesReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      objectPath;
  MsgSegment      userRole;
} EnumInstanceNamesReq;
#define EIN_REQ_REG_SEGMENTS 3

typedef struct enumInstancesReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      objectPath;
  MsgSegment      userRole;
  MsgSegment      properties[1];
} EnumInstancesReq;
#define EI_REQ_REG_SEGMENTS 3

typedef struct execQueryReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      objectPath;
  MsgSegment      query;
  MsgSegment      queryLang;
  MsgSegment      userRole;
} ExecQueryReq;
#define EQ_REQ_REG_SEGMENTS 5

typedef struct associatorsReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      objectPath;
  MsgSegment      resultClass;
  MsgSegment      role;
  MsgSegment      assocClass;
  MsgSegment      resultRole;
  MsgSegment      userRole;
  MsgSegment      properties[1];
} AssociatorsReq;
#define AI_REQ_REG_SEGMENTS 7

typedef struct referencesReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      objectPath;
  MsgSegment      resultClass;
  MsgSegment      role;
  MsgSegment      userRole;
  MsgSegment      properties[1];
} ReferencesReq;
#define RI_REQ_REG_SEGMENTS 5

typedef struct associatorNamesReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      objectPath;
  MsgSegment      resultClass;
  MsgSegment      role;
  MsgSegment      assocClass;
  MsgSegment      resultRole;
  MsgSegment      userRole;
} AssociatorNamesReq;
#define AIN_REQ_REG_SEGMENTS 7

typedef struct referenceNamesReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      objectPath;
  MsgSegment      resultClass;
  MsgSegment      role;
  MsgSegment      userRole;
} ReferenceNamesReq;
#define RIN_REQ_REG_SEGMENTS 5

typedef struct getInstanceReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      objectPath;
  MsgSegment      userRole;
  MsgSegment      properties[1];
} GetInstanceReq;
#define GI_REQ_REG_SEGMENTS 3

typedef struct createClassReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      path;
  MsgSegment      cls;
  MsgSegment      userRole;
} CreateClassReq;
#define CC_REQ_REG_SEGMENTS 4

typedef struct createInstanceReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      path;
  MsgSegment      instance;
  MsgSegment      userRole;
} CreateInstanceReq;
#define CI_REQ_REG_SEGMENTS 4

typedef struct modifyInstanceReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      path;
  MsgSegment      instance;
  MsgSegment      userRole;
  MsgSegment      properties[1];
} ModifyInstanceReq;
#define MI_REQ_REG_SEGMENTS 4

typedef struct deleteInstanceReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      objectPath;
  MsgSegment      userRole;
} DeleteInstanceReq;
#define DI_REQ_REG_SEGMENTS 3

typedef struct deleteClassReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      objectPath;
  MsgSegment      userRole;
} DeleteClassReq;
#define DC_REQ_REG_SEGMENTS 3

typedef struct invokeMethodReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      objectPath;
  MsgSegment      method;
  MsgSegment      in;
  MsgSegment      out;
  MsgSegment      userRole;
} InvokeMethodReq;
#define IM_REQ_REG_SEGMENTS 6

typedef struct loadProviderReq {
  BinRequestHdr   hdr;
  MsgSegment      className;
  MsgSegment      libName;
  MsgSegment      provName;
  MsgSegment      parameters;
  unsigned int    unload;
} LoadProviderReq;

typedef struct indicationReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      objectPath;
  MsgSegment      query;
  MsgSegment      language;
  MsgSegment      type;
  MsgSegment      sns;
  void           *filterId;
} IndicationReq;

typedef struct enumQualifiersReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      path;
} EnumQualifiersReq;

typedef struct setQualifierReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      path;
  MsgSegment      qualifier;
} SetQualifierReq;

typedef struct getQualifierReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      path;
} GetQualifierReq;

typedef struct deleteQualifierReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      path;
} DeleteQualifierReq;

typedef struct getPropertyReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      path;
  MsgSegment      name;
} GetPropertyReq;

typedef struct setPropertyReq {
  BinRequestHdr   hdr;
  MsgSegment      principal;
  MsgSegment      path;
  MsgSegment      inst;         /* we abuse an instance with a single
                                 * property, so we do not have to create a 
                                 * * property msg segment */
} SetPropertyReq;

int             getProviderContext(BinRequestContext * ctx);
BinResponseHdr **invokeProviders(BinRequestContext * binCtx, int *err,
                                 int *count);
BinResponseHdr *invokeProvider(BinRequestContext * ctx);
void            freeResponseHeaders(BinResponseHdr ** resp,
                                    BinRequestContext * ctx);
sigset_t mask, old_mask;

#endif
/* MODELINES */
/* DO NOT EDIT BELOW THIS COMMENT */
/* Modelines are added by 'make pretty' */
/* -*- Mode: C; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* vi:set ts=2 sts=2 sw=2 expandtab: */
