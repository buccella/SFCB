/*
 * $id$
 *
 * © Copyright IBM Corp. 2007
 *
 * THIS FILE IS PROVIDED UNDER THE TERMS OF THE ECLIPSE PUBLIC LICENSE
 * ("AGREEMENT"). ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS FILE
 * CONSTITUTES RECIPIENTS ACCEPTANCE OF THE AGREEMENT.
 *
 * You can obtain a current copy of the Eclipse Public License from
 * http://www.opensource.org/licenses/eclipse-1.0.php
 *
 * Author:        Sven Schuetz
 *
 * Description:
 *
 * instance utility functions
 *
 */

#ifndef _INSTANCE_H
#define _INSTANCE_H

int             instanceCompare(CMPIInstance *inst1, CMPIInstance *inst2);
const char     *instGetClassName(CMPIInstance *ci);
const char     *instGetNameSpace(CMPIInstance *ci);
CMPIStatus      filterFlagProperty(CMPIInstance* ci, const char* id);
void            setCCN(CMPIObjectPath * cop, CMPIInstance *ci, const char * sccn);

#endif
/* MODELINES */
/* DO NOT EDIT BELOW THIS COMMENT */
/* Modelines are added by 'make pretty' */
/* -*- Mode: C; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* vi:set ts=2 sts=2 sw=2 expandtab: */
