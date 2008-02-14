/* imfile.c
 * 
 * This is the input module for reading text file data. A text file is a
 * non-binary file who's lines are delemited by the \n character.
 *
 * Work originally begun on 2008-02-01 by Rainer Gerhards
 *
 * Copyright 2008 Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Rsyslog is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Rsyslog is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Rsyslog.  If not, see <http://www.gnu.org/licenses/>.
 *
 * A copy of the GPL can be found in the file "COPYING" in this distribution.
 */
#include "config.h" /* this is for autotools and always must be the first include */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>		/* do NOT remove: will soon be done by the module generation macros */
#include <fcntl.h>		/* do NOT remove: will soon be done by the module generation macros */
#include "rsyslog.h"		/* error codes etc... */
#include "syslogd.h"
#include "cfsysline.h"		/* access to config file objects */
#include "module-template.h"	/* generic module interface code - very important, read it! */
#include "srUtils.h"		/* some utility functions */
#include "msg.h"
#include "stream.h"

MODULE_TYPE_INPUT	/* must be present for input modules, do not remove */

/* defines */

/* Module static data */
DEF_IMOD_STATIC_DATA	/* must be present, starts static data */

/* Here, define whatever static data is needed. Is it suggested that static variables only are
 * used (not externally visible). If you need externally visible variables, make sure you use a
 * prefix in order not to conflict with other modules or rsyslogd itself (also see comment
 * at file header).
 */

typedef struct fileInfo_s {
	uchar *pszFileName;
	uchar *pszTag;
	uchar *pszStateFile; /* file in which state between runs is to be stored */
	int64 offsLast; /* offset last read from */
	int iFacility;
	int iSeverity;
	strm_t *pStrm;	/* its stream (NULL if not assigned) */
} fileInfo_t;


/* config variables */
static uchar *pszFileName = NULL;
static uchar *pszFileTag = NULL;
static uchar *pszStateFile = NULL;
static int iPollInterval = 10;	/* number of seconds to sleep when there was no file activity */
static int iFacility;
static int iSeverity;

static int iFilPtr = 0;
#define MAX_INPUT_FILES 100
static fileInfo_t files[MAX_INPUT_FILES];

/* instanceData must be defined to keep the framework happy, but it currently
 * is of no practical use. This may change in later revisions of the plugin
 * interface.
 */
typedef struct _instanceData {
} instanceData;

/* config settings */


/* enqueue the read file line as a message
 */
static rsRetVal enqLine(fileInfo_t *pInfo, rsCStrObj *cstrLine)
{
		DEFiRet;
		msg_t *pMsg;

		CHKiRet(msgConstruct(&pMsg));
		MsgSetUxTradMsg(pMsg, (char*)rsCStrGetSzStr(cstrLine));
		MsgSetRawMsg(pMsg, (char*)rsCStrGetSzStr(cstrLine));
		MsgSetMSG(pMsg, (char*)rsCStrGetSzStr(cstrLine));
		MsgSetHOSTNAME(pMsg, LocalHostName);
		MsgSetTAG(pMsg, (char*)pInfo->pszTag);
		pMsg->iFacility = pInfo->iFacility;
		pMsg->iSeverity = pInfo->iSeverity;
		pMsg->bParseHOSTNAME = 0;
		getCurrTime(&(pMsg->tTIMESTAMP)); /* use the current time! */
		CHKiRet(submitMsg(pMsg));
finalize_it:
	RETiRet;
}


/* try to open a file. This involves checking if there is a status file and,
 * if so, reading it in. Processing continues from the last know location.
 */
static rsRetVal
openFile(fileInfo_t *pThis)
{
	DEFiRet;
	strm_t *psSF = NULL;
	uchar pszSFNam[MAXFNAME];
	size_t lenSFNam;
	struct stat stat_buf;

	/* Construct file name */
	lenSFNam = snprintf((char*)pszSFNam, sizeof(pszSFNam) / sizeof(uchar), "%s/%s",
			     (char*) glblGetWorkDir(), (char*)pThis->pszStateFile);

	/* check if the file exists */
	if(stat((char*) pszSFNam, &stat_buf) == -1) {
		if(errno == ENOENT) {
			/* currently no object! dbgoprint((obj_t*) pThis, "clean startup, no .si file found\n"); */
			ABORT_FINALIZE(RS_RET_FILE_NOT_FOUND);
		} else {
			/* currently no object! dbgoprint((obj_t*) pThis, "error %d trying to access .si file\n", errno); */
			ABORT_FINALIZE(RS_RET_IO_ERROR);
		}
	}

	/* If we reach this point, we have a .si file */

	CHKiRet(strmConstruct(&psSF));
	CHKiRet(strmSettOperationsMode(psSF, STREAMMODE_READ));
	CHKiRet(strmSetsType(psSF, STREAMTYPE_FILE_SINGLE));
	CHKiRet(strmSetFName(psSF, pszSFNam, lenSFNam));
	CHKiRet(strmConstructFinalize(psSF));

	/* read back in the object */
	CHKiRet(objDeserialize(&pThis->pStrm, OBJstrm, psSF, NULL, pThis));

	CHKiRet(strmSeekCurrOffs(pThis->pStrm));

	/* OK, we could successfully read the file, so we now can request that it be deleted.
	 * If we need it again, it will be written on the next shutdown.
	 */
	psSF->bDeleteOnClose = 1;

finalize_it:
	if(psSF != NULL)
		strmDestruct(&psSF);

	if(iRet != RS_RET_OK) {
		CHKiRet(strmConstruct(&pThis->pStrm));
		CHKiRet(strmSettOperationsMode(pThis->pStrm, STREAMMODE_READ));
		CHKiRet(strmSetsType(pThis->pStrm, STREAMTYPE_FILE_MONITOR));
		CHKiRet(strmSetFName(pThis->pStrm, pThis->pszFileName, strlen((char*) pThis->pszFileName)));
		CHKiRet(strmConstructFinalize(pThis->pStrm));
	}

	RETiRet;
}


/* poll a file, need to check file rollover etc. open file if not open */
static rsRetVal pollFile(fileInfo_t *pThis)
{
	DEFiRet;
	rsCStrObj *pCStr;

	if(pThis->pStrm == NULL) {
		CHKiRet(openFile(pThis)); /* open file */
	}

	/* loop below will be exited when strmReadLine() returns EOF */
	while(1) {
		CHKiRet(strmReadLine(pThis->pStrm, &pCStr));
		CHKiRet(enqLine(pThis, pCStr)); /* process line */
	}

finalize_it:
	RETiRet;
}


/* This function is the cancel cleanup handler. It is called when rsyslog decides the
 * module must be stopped, what most probably happens during shutdown of rsyslogd. When
 * this function is called, the runInput() function (below) is already terminated - somewhere
 * in the middle of what it was doing. The cancel cleanup handler below should take
 * care of any locked mutexes and such, things that really need to be cleaned up
 * before processing continues. In general, many plugins do not need to provide
 * any code at all here.
 *
 * IMPORTANT: the calling interface of this function can NOT be modified. It actually is
 * called by pthreads. The provided argument is currently not being used.
 */
/* ------------------------------------------------------------------------------------------ *
 * DO NOT TOUCH the following code - it will soon be part of the module generation macros!    */
static void
inputModuleCleanup(void __attribute__((unused)) *arg)
{
	BEGINfunc
/* END no-touch zone                                                                          *
 * ------------------------------------------------------------------------------------------ */



	/* so far not needed */



/* ------------------------------------------------------------------------------------------ *
 * DO NOT TOUCH the following code - it will soon be part of the module generation macros!    */
	ENDfunc
}
/* END no-touch zone                                                                          *
 * ------------------------------------------------------------------------------------------ */


/* This function is called by the framework to gather the input. The module stays
 * most of its lifetime inside this function. It MUST NEVER exit this function. Doing
 * so would end module processing and rsyslog would NOT reschedule the module. If
 * you exit from this function, you violate the interface specification!
 *
 * So how is it terminated? When it is time to terminate, rsyslog actually cancels
 * the threads. This may sound scary, but is not. There is a cancel cleanup handler
 * defined (the function directly above). See comments there for specifics.
 *
 * runInput is always called on a single thread. If the module neees multiple threads,
 * it is free to create them. HOWEVER, it must make sure that any threads created
 * are killed and joined in the cancel cleanup handler.
 */
BEGINrunInput
	int i;
CODESTARTrunInput
	/* ------------------------------------------------------------------------------------------ *
	 * DO NOT TOUCH the following code - it will soon be part of the module generation macros!    */
	pthread_cleanup_push(inputModuleCleanup, NULL);
	while(1) { /* endless loop - do NOT break; out of it! */
	/* END no-touch zone                                                                          *
	 * ------------------------------------------------------------------------------------------ */

	for(i = 0 ; i < iFilPtr ; ++i) {
		pollFile(&files[i]);
	}

RUNLOG_VAR("%d", iPollInterval);
	/* Note: the 10ns additional wait is vitally important. It guards rsyslog against totally
	 * hogging the CPU if the users selects a polling interval of 0 seconds. It doesn't hurt any
	 * other valid scenario. So do not remove. -- rgerhards, 2008-02-14
	 */
	srSleep(iPollInterval, 10);

	/* ------------------------------------------------------------------------------------------ *
	 * DO NOT TOUCH the following code - it will soon be part of the module generation macros!    */
	}
	/*NOTREACHED*/
	
	pthread_cleanup_pop(0); /* just for completeness, but never called... */
	RETiRet;	/* use it to make sure the housekeeping is done! */
ENDrunInput
	/* END no-touch zone                                                                          *
	 * ------------------------------------------------------------------------------------------ */


/* The function is called by rsyslog before runInput() is called. It is a last chance
 * to set up anything specific. Most importantly, it can be used to tell rsyslog if the
 * input shall run or not. The idea is that if some config settings (or similiar things)
 * are not OK, the input can tell rsyslog it will not execute. To do so, return
 * RS_RET_NO_RUN or a specific error code. If RS_RET_OK is returned, rsyslog will
 * proceed and call the runInput() entry point.
 */
BEGINwillRun
CODESTARTwillRun
	if(iFilPtr == 0) {
		logerror("No files configured to be monitored");
		ABORT_FINALIZE(RS_RET_NO_RUN);
	}

finalize_it:
ENDwillRun



/* This function persists information for a specific file being monitored.
 * To do so, it simply persists the stream object. We do NOT abort on error
 * iRet as that makes matters worse (at least we can try persisting the others...).
 * rgerhards, 2008-02-13
 */
static rsRetVal
persistStrmState(fileInfo_t *pInfo)
{
	DEFiRet;
	strm_t *psSF = NULL; /* state file (stream) */

	ASSERT(pInfo != NULL);

dbgprintf("persistStrmState: dir %s, file %s\n", glblGetWorkDir(), pInfo->pszStateFile);
	/* TODO: create a function persistObj in obj.c? */
	CHKiRet(strmConstruct(&psSF));
	CHKiRet(strmSetDir(psSF, glblGetWorkDir(), strlen((char*)glblGetWorkDir())));
	CHKiRet(strmSettOperationsMode(psSF, STREAMMODE_WRITE));
	CHKiRet(strmSetiAddtlOpenFlags(psSF, O_TRUNC));
	CHKiRet(strmSetsType(psSF, STREAMTYPE_FILE_SINGLE));
	CHKiRet(strmSetFName(psSF, pInfo->pszStateFile, strlen((char*) pInfo->pszStateFile)));
	CHKiRet(strmConstructFinalize(psSF));

	CHKiRet(strmSerialize(pInfo->pStrm, psSF));

	CHKiRet(strmDestruct(&psSF));

finalize_it:
	RETiRet;
}


/* This function is called by the framework after runInput() has been terminated. It
 * shall free any resources and prepare the module for unload.
 *
 * So it is important that runInput() keeps track of what needs to be cleaned up.
 * Objects to think about are files (must be closed), network connections, threads (must
 * be stopped and joined) and memory (must be freed). Of course, there are a myriad
 * of other things, so use your own judgement what you need to do.
 *
 * Another important chore of this function is to persist whatever state the module
 * needs to persist. Unfortunately, there is currently no standard way of doing that.
 * Future version of the module interface will probably support it, but that doesn't
 * help you right at the moment. In general, it is suggested that anything that needs
 * to be persisted is saved in a file, whose name and location is passed in by a
 * module-specific config directive.
 */
BEGINafterRun
	int i;
CODESTARTafterRun
	/* loop through file array and close everything that's open */

	/* persist file state information. We do NOT abort on error iRet as that makes
	 * matters worse (at least we can try persisting the others...).
	 */
	for(i = 0 ; i < iFilPtr ; ++i) {
		persistStrmState(&files[i]);
	}
ENDafterRun


/* The following entry points are defined in module-template.h.
 * In general, they need to be present, but you do NOT need to provide
 * any code here.
 */
BEGINfreeInstance
CODESTARTfreeInstance
ENDfreeInstance


BEGINdbgPrintInstInfo
CODESTARTdbgPrintInstInfo
ENDdbgPrintInstInfo


BEGINmodExit
CODESTARTmodExit
ENDmodExit


BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_IMOD_QUERIES
ENDqueryEtryPt


/* The following function shall reset all configuration variables to their
 * default values. The code provided in modInit() below registers it to be
 * called on "$ResetConfigVariables". You may also call it from other places,
 * but in general this is not necessary. Once runInput() has been called, this
 * function here is never again called.
 */
static rsRetVal resetConfigVariables(uchar __attribute__((unused)) *pp, void __attribute__((unused)) *pVal)
{
	DEFiRet;

	if(pszFileName != NULL) {
		free(pszFileName);
		pszFileName = NULL;
	}

	if(pszFileTag != NULL) {
		free(pszFileTag);
		pszFileTag = NULL;
	}

	if(pszStateFile != NULL) {
		free(pszFileTag);
		pszFileTag = NULL;
	}


	/* set defaults... */
	iPollInterval = 10;
	iFacility = 12; /* see RFC 3164 for values */
	iSeverity = 4;

	RETiRet;
}


/* add a new monitor */
static rsRetVal addMonitor(void __attribute__((unused)) *pVal, uchar __attribute__((unused)) *pNewVal)
{
	DEFiRet;
	fileInfo_t *pThis;

	if(iFilPtr < MAX_INPUT_FILES) {
		pThis = &files[iFilPtr];
		++iFilPtr;
		/* TODO: check for strdup() NULL return */
		if(pszFileName != NULL)
			pThis->pszFileName = (uchar*) strdup((char*) pszFileName);
		if(pszFileTag != NULL)
			pThis->pszTag = (uchar*) strdup((char*) pszFileTag);
		if(pszStateFile != NULL)
			pThis->pszStateFile = (uchar*) strdup((char*) pszStateFile);
		pThis->iSeverity = iSeverity;
		pThis->iFacility = iFacility;
		pThis->offsLast = 0;
	} else {
		logerror("Too many file monitors configured - ignoring this one");
	}
	RETiRet;
}

/* modInit() is called once the module is loaded. It must perform all module-wide
 * initialization tasks. There are also a number of housekeeping tasks that the
 * framework requires. These are handled by the macros. Please note that the
 * complexity of processing is depending on the actual module. However, only
 * thing absolutely necessary should be done here. Actual app-level processing
 * is to be performed in runInput(). A good sample of what to do here may be to
 * set some variable defaults. The most important thing probably is registration
 * of config command handlers.
 */
BEGINmodInit()
CODESTARTmodInit
	*ipIFVersProvided = 1; /* interface spec version this module is written to (currently always 1) */
CODEmodInit_QueryRegCFSLineHdlr
	 CHKiRet(omsdRegCFSLineHdlr((uchar *)"inputfilename", 0, eCmdHdlrGetWord,
	  	NULL, &pszFileName, STD_LOADABLE_MODULE_ID));
	 CHKiRet(omsdRegCFSLineHdlr((uchar *)"inputfiletag", 0, eCmdHdlrGetWord,
	  	NULL, &pszFileTag, STD_LOADABLE_MODULE_ID));
	 CHKiRet(omsdRegCFSLineHdlr((uchar *)"inputfilestatefile", 0, eCmdHdlrGetWord,
	  	NULL, &pszStateFile, STD_LOADABLE_MODULE_ID));
	 /* use numerical values as of RFC 3164 for the time being... */
	 CHKiRet(omsdRegCFSLineHdlr((uchar *)"inputfileseverity", 0, eCmdHdlrInt,
	  	NULL, &iSeverity, STD_LOADABLE_MODULE_ID));
	 CHKiRet(omsdRegCFSLineHdlr((uchar *)"inputfilefacility", 0, eCmdHdlrInt,
	  	NULL, &iFacility, STD_LOADABLE_MODULE_ID));
	 CHKiRet(omsdRegCFSLineHdlr((uchar *)"inputfilepollinterval", 0, eCmdHdlrInt,
	  	NULL, &iPollInterval, STD_LOADABLE_MODULE_ID));
	/* that command ads a new file! */
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"inputrunfilemonitor", 0, eCmdHdlrGetWord,
		addMonitor, NULL, STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"resetconfigvariables", 1, eCmdHdlrCustomHandler,
		resetConfigVariables, NULL, STD_LOADABLE_MODULE_ID));
ENDmodInit
/*
 * vim:set ai:
 */
