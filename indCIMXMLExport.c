
/*
 * $Id: indCIMXMLExport.c,v 1.14 2010/01/21 21:50:37 mchasal Exp $
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
 *
 * Description:
 *
 * CIM XML export support for indications.
 *
 */

#include <curl/curl.h>
#include <sfcCommon/utilft.h>
#include "trace.h"
#include <stdlib.h>
#include <string.h>
#include "control.h"

extern UtilStringBuffer *newStringBuffer(int);
extern int      getControlChars(char *id, char **val);

// These are the constant headers added to all requests
static const char *headers[] = {
  "Content-type: application/xml; charset=\"utf-8\"",
  "Connection: Keep-Alive, TE",
  "CIMProtocolVersion: 1.0",
  "CIMExport: MethodRequest",
  "CIMExportMethod: ExportIndication"
};
#define NUM_HEADERS ((sizeof(headers))/(sizeof(headers[0])))

// 
// NOTE:
// 
// All strings passed to libcurl are not copied so they must have a
// lifetime
// at least as long as the last function call made on the curl handle. The
// only exception to this seems to be strings passed to the
// curl_slist_append
// function which are copied.
// 

typedef struct curlData {
  // The handle to the curl object
  CURL           *mHandle;
  // The list of headers sent with each request
  struct curl_slist *mHeaders;
  // The body of the request
  UtilStringBuffer *mBody;
  // The uri of the request
  UtilStringBuffer *mUri;
  // The username/password used in authentication
  char           *mUserPass;
  // Used to store the HTTP response
  UtilStringBuffer *mResponse;
  /*
   * bool supportsSSL(); void genRequest(URL &url, char *op, bool
   * cls=false, bool keys=false); void addPayload(const string& pl);
   * string getResponse();
   * 
   * // Initializes the HTTP headers void initializeHeaders(); 
   */
} CurlData;

static void
init(CurlData * cd)
{
  cd->mHandle = curl_easy_init();
  cd->mHeaders = NULL;
  cd->mResponse = NULL;
  cd->mBody = newStringBuffer(4096);
  cd->mUserPass = NULL;
}

static void
uninit(CurlData * cd)
{
  if (cd->mHandle)
    curl_easy_cleanup(cd->mHandle);
  if (cd->mHeaders)
    curl_slist_free_all(cd->mHeaders);
  cd->mBody->ft->release(cd->mBody);
  if (cd->mResponse)
    cd->mResponse->ft->release(cd->mResponse);
}

static int
supportsSSL()
{
#if LIBCURL_VERSION_NUM >= 0x071000
  static curl_version_info_data *version = NULL;

  if (version == NULL) {
    version = curl_version_info(CURLVERSION_NOW);
  }

  if (version && (version->features & CURL_VERSION_SSL))
    return 1;
  else
    return 0;
#else
  // Assume we support SSL if we don't have the curl_version_info API
  return 1;
#endif
}

static void
initializeHeaders(CurlData * cd)
{
  unsigned int    i;
  // Free any pre-existing headers
  if (cd->mHeaders) {
    curl_slist_free_all(cd->mHeaders);
    cd->mHeaders = NULL;
  }
  // Add all of the common headers
  for (i = 0; i < NUM_HEADERS; i++)
    cd->mHeaders = curl_slist_append(cd->mHeaders, headers[i]);
}

static          size_t
writeCb(void *ptr, size_t size, size_t nmemb, void *stream)
{
  UtilStringBuffer *sb = (UtilStringBuffer *) stream;
  unsigned int    length = 0;
  unsigned long long calcLength = (unsigned long) size * nmemb;
  if (calcLength > UINT_MAX) {
    mlogf(M_ERROR, M_SHOW,
          "--- Cannot allocate for %d members of size $d\n", nmemb, size);
    return 0;
  }
  length = calcLength & UINT_MAX;
  char            c = ((char *) ptr)[length];
  ((char *) ptr)[length] = 0;
  sb->ft->appendChars(sb, (char *) ptr);
  ((char *) ptr)[length] = c;
  return length;
}

static int
genRequest(CurlData * cd, char *url, char **msg)
{
  CURLcode        rv;
  char           *fnc,
                 *fnk,
                 *fnt,
                 *fnl;

  *msg = NULL;

  if (!cd->mHandle) {
    *msg = strdup("Unable to initialize curl interface.");
    return 1;
  }

  if (!supportsSSL() && (strncasecmp(url, "https:", 6) == 0)) {
    *msg = strdup("This curl library does not support https urls.");
    return 2;
  }

  /*
   * Initialize curl with the url 
   */
  rv = curl_easy_setopt(cd->mHandle, CURLOPT_URL, url);

  /*
   * Disable progress output 
   */
  rv = curl_easy_setopt(cd->mHandle, CURLOPT_NOPROGRESS, 1);

  /*
   * This will be a HTTP post 
   */
  rv = curl_easy_setopt(cd->mHandle, CURLOPT_POST, 1);


  /*
   * Enable endpoint cert verification as required
   */
  getControlChars("sslIndicationReceiverCert", &fnl);
  for(;;) {
    if (strcasecmp(fnl, "ignore") == 0) {
      rv = curl_easy_setopt(cd->mHandle, CURLOPT_SSL_VERIFYPEER, 0);
      rv = curl_easy_setopt(cd->mHandle, CURLOPT_SSL_VERIFYHOST, 0);
      break;
    } else if ((strcasecmp(fnl, "verify") == 0) ||
               (strcasecmp(fnl, "verifyhostname") == 0)) {
      if (getControlChars("sslClientTrustStore", &fnt) == 0) {
        rv = curl_easy_setopt(cd->mHandle, CURLOPT_CAINFO, fnt);
      } else {
        /* possible? */
        *msg=strdup("Cannot determine value of sslClientTrustStore parameter.");
        return 3;
      }
      rv = curl_easy_setopt(cd->mHandle, CURLOPT_SSL_VERIFYPEER, 1);
      if (strcasecmp(fnl, "verify") == 0) {
        rv = curl_easy_setopt(cd->mHandle, CURLOPT_SSL_VERIFYHOST, 0);
        break;
      } else {  /* verifyhostname */
        rv = curl_easy_setopt(cd->mHandle, CURLOPT_SSL_VERIFYHOST, 2);
        break;
      }
    } else {
      // Since we don't know user intent in this case, assume the strictest.
      mlogf(M_ERROR,M_SHOW,
          "--- ERROR: Invalid value for sslIndicationReceiverCert, setting to: verifyhostname.\n");
      fnl = "verifyhostname";
    }
  }

  /*
   * set up client side cert usage 
   */
  if ((getControlChars("sslCertificateFilePath", &fnc) == 0) &&
      (getControlChars("sslKeyFilePath", &fnk) == 0)) {
    rv = curl_easy_setopt(cd->mHandle, CURLOPT_SSLKEY, fnk);
    rv = curl_easy_setopt(cd->mHandle, CURLOPT_SSLCERT, fnc);
  } else {
    *msg =
        strdup
        ("Failed to get cert path and/or key file information for client side cert usage.");
    return 3;
  }

  /* Set transfer timeout to config option or 10 sec */
  long indicationCurlTimeout=10;
  if (getControlNum("indicationCurlTimeout", &indicationCurlTimeout))
        indicationCurlTimeout = 10;
  rv = curl_easy_setopt(cd->mHandle, CURLOPT_TIMEOUT, indicationCurlTimeout);

  /*
   * Set username and password * / if (url.user.length() > 0 &&
   * url.password.length() > 0) { mUserPass = url.user + ":" +
   * url.password; rv = curl_easy_setopt(mHandle, CURLOPT_USERPWD,
   * mUserPass.c_str()); } 
   */

  // Initialize default headers
  initializeHeaders(cd);

  // Set all of the headers for the request
  rv = curl_easy_setopt(cd->mHandle, CURLOPT_HTTPHEADER, cd->mHeaders);

  // Set up the callbacks to store the response
  rv = curl_easy_setopt(cd->mHandle, CURLOPT_WRITEFUNCTION, writeCb);

  // Use CURLOPT_FILE instead of CURLOPT_WRITEDATA - more portable
  cd->mResponse = newStringBuffer(4096);
  rv = curl_easy_setopt(cd->mHandle, CURLOPT_FILE, cd->mResponse);

  // Fail if we receive an error (HTTP response code >= 300)
  rv = curl_easy_setopt(cd->mHandle, CURLOPT_FAILONERROR, 1);

  rv = curl_easy_setopt(cd->mHandle, CURLOPT_NOSIGNAL, 1);

  char *curldebug = getenv("CURLDEBUG");
  if (curldebug && strcasecmp(curldebug,"false"))
    rv = curl_easy_setopt(cd->mHandle, CURLOPT_VERBOSE, 1);

  if (rv) {
    *msg = strdup("Some curl opts failed during setup");
    return 2;
  }

  return 0;
}

char           *curlMsgs[] = {
  "OK",
  "Unsupported protocol. This build of curl has no support for this protocol",
  "Failed to initialize.",
  "URL malformat. The syntax was not correct.",
  "URL user malformatted. The user-part of the URL syntax  was  not correct.",
  "Couldn’t  resolve  proxy.  The  given  proxy  host  could not be resolved.",
  "Couldn’t resolve host. The given remote host was not resolved.",
  /*
   * 7
   */ "Failed to connect to host.",
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  /*
   * 22
   */ "HTTP  page  not  retrieved.  The  requested url was not found or returned another "
      "error with the HTTP error  code  being  400  or above.",
  "Write  error.  Curl couldn’t write data to a local filesystem or similar."
};

static char    *
getErrorMessage(CURLcode err)
{
  char            error[4096];
#if LIBCURL_VERSION_NUM >= 0x071200
  // error = curl_easy_strerror(err);
  snprintf(error, 4095, "CURL error: %d (%s)", err,
           curl_easy_strerror(err));
#else
  if ((err > 0 && err < 8) || (err >= 22 && err <= 23))
    snprintf(error, 4095, "CURL error: %d (%s)", err, curlMsgs[err]);
  else
    snprintf(error, 4095, "CURL error: %d", err);
#endif
  return strdup(error);
}

static int
getResponse(CurlData * cd, char **msg)
{
  CURLcode        rv;
  int             rc = 0;

  rv = curl_easy_perform(cd->mHandle);
  if (rv) {
    long            responseCode = -1;
    char           *error;
    // Use CURLINFO_HTTP_CODE instead of CURLINFO_RESPONSE_CODE
    // (more portable to older versions of curl)
    curl_easy_getinfo(cd->mHandle, CURLINFO_HTTP_CODE, &responseCode);
    switch(responseCode) {
      case 200:
         rc = 0; /* HTTP 200 is OK. set rc to 0 */
         break;
      case 400:
         *msg = strdup("Bad Request");
         rc = 400;
         break;
      case 401:
         error = (cd->mUserPass) ? "Invalid username/password." :
                                   "Username/password required.";
         *msg = strdup(error);
         rc = 401;
         break;
      case 501:
         *msg = strdup("Not Implemented");
         rc = 501;
         break;
      default:
         if (responseCode != 0) {
           // If we got an unrecognized response code, return it
           rc = (int)responseCode;
         } else {
           // Otherwise there was some other curl error, return that
           rc = (int)rv;
         }
         *msg = getErrorMessage(rv);
         break;
    } 
    return rc;
  }

  if (cd->mResponse->ft->getSize(cd->mResponse) == 0) {
    rc = 5;
    *msg = strdup("No data received from server.");
  }
  // if (dumpXml)
  // cerr << "From server: " << response << endl;
  return 0;
}

static int
addPayload(CurlData * cd, char *pl, char **msg)
{
  CURLcode        rv;

  cd->mBody->ft->appendChars(cd->mBody, pl);
  rv = curl_easy_setopt(cd->mHandle, CURLOPT_POSTFIELDS,
                        cd->mBody->ft->getCharPtr(cd->mBody));

  if (rv) {
    *msg = getErrorMessage(rv);
    return 6;
  }

  else {
    rv = curl_easy_setopt(cd->mHandle, CURLOPT_POSTFIELDSIZE,
                          cd->mBody->ft->getSize(cd->mBody));
    if (rv) {
      *msg = getErrorMessage(rv);
      return 7;
    }
  }
  return 0;
}

int
exportIndication(char *url, char *payload, char **resp, char **msg)
{
  CurlData        cd;
  int             rc = 0;
  FILE           *out;

  *msg = NULL;
  *resp = NULL;

  _SFCB_ENTER(TRACE_INDPROVIDER, "exportIndication");

  if (strncasecmp(url, "file://", 7) == 0) {
    out = fopen(url + 7, "a+");
    if (out) {
      fprintf(out, "%s\n", payload);
      fprintf(out, "=========== End of Indication ===========\n");
      fclose(out);
    } else {
      rc = 1;
      mlogf(M_ERROR, M_SHOW,
            "Unable to open file to process indication: %s\n", url);
      _SFCB_TRACE(1, ("--- Unable to open file: %s", url));
    }
    _SFCB_RETURN(rc);
  }

  init(&cd);
  if ((rc = genRequest(&cd, url, msg)) == 0) {
    if ((rc = addPayload(&cd, payload, msg)) == 0) {
      if ((rc = getResponse(&cd, msg)) == 0) {
        *resp = strdup(cd.mResponse->ft->getCharPtr(cd.mResponse));
      }
    }
  }

  _SFCB_TRACE(1, ("--- url: %s rc: %d %s", url, rc, *msg));
  if (rc) {
    mlogf(M_ERROR, M_SHOW,
          "Problem processing indication to %s. sfcb rc: %d %s\n", url, rc,
          *msg);
  }

  uninit(&cd);

  _SFCB_RETURN(rc);
}
/* MODELINES */
/* DO NOT EDIT BELOW THIS COMMENT */
/* Modelines are added by 'make pretty' */
/* -*- Mode: C; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* vi:set ts=2 sts=2 sw=2 expandtab: */
