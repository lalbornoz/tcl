/*
 * tclIO.c --
 *
 *	This file provides the generic portions (those that are the same on
 *	all platforms and for all channel types) of Tcl's IO facilities.
 *
 * Copyright (c) 1998-2000 Ajuba Solutions
 * Copyright (c) 1995-1997 Sun Microsystems, Inc.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: tclIO.c,v 1.68.2.37 2008/04/15 19:19:29 dgp Exp $
 */

#include "tclInt.h"
#include "tclIO.h"
#include <assert.h>

/*
 * All static variables used in this file are collected into a single instance
 * of the following structure. For multi-threaded implementations, there is
 * one instance of this structure for each thread.
 *
 * Notice that different structures with the same name appear in other files.
 * The structure defined below is used in this file only.
 */

typedef struct ThreadSpecificData {
    NextChannelHandler *nestedHandlerPtr;
				/* This variable holds the list of nested
				 * ChannelHandlerEventProc invocations. */
    ChannelState *firstCSPtr;	/* List of all channels currently open,
				 * indexed by ChannelState, as only one
				 * ChannelState exists per set of stacked
				 * channels. */
    Tcl_Channel stdinChannel;	/* Static variable for the stdin channel. */
    int stdinInitialized;
    Tcl_Channel stdoutChannel;	/* Static variable for the stdout channel. */
    int stdoutInitialized;
    Tcl_Channel stderrChannel;	/* Static variable for the stderr channel. */
    int stderrInitialized;
    Tcl_Encoding binaryEncoding;
} ThreadSpecificData;

static Tcl_ThreadDataKey dataKey;

/*
 * Static functions in this file:
 */

static ChannelBuffer *	AllocChannelBuffer(int length);
static void		ChannelTimerProc(ClientData clientData);
static int		CheckChannelErrors(ChannelState *statePtr,
			    int direction);
static int		CheckFlush(Channel *chanPtr, ChannelBuffer *bufPtr,
			    int newlineFlag);
static int		CheckForDeadChannel(Tcl_Interp *interp,
			    ChannelState *statePtr);
static void		CheckForStdChannelsBeingClosed(Tcl_Channel chan);
static void		CleanupChannelHandlers(Tcl_Interp *interp,
			    Channel *chanPtr);
static int		CloseChannel(Tcl_Interp *interp, Channel *chanPtr,
			    int errorCode);
static void		CommonGetsCleanup(Channel *chanPtr);
static int		CopyAndTranslateBuffer(ChannelState *statePtr,
			    char *result, int space);
static int		CopyBuffer(Channel *chanPtr, char *result, int space);
static int		CopyData(CopyState *csPtr, int mask);
static void		CopyEventProc(ClientData clientData, int mask);
static void		CreateScriptRecord(Tcl_Interp *interp,
			    Channel *chanPtr, int mask, Tcl_Obj *scriptPtr);
static void		DeleteChannelTable(ClientData clientData,
			    Tcl_Interp *interp);
static void		DeleteScriptRecord(Tcl_Interp *interp,
			    Channel *chanPtr, int mask);
static int		DetachChannel(Tcl_Interp *interp, Tcl_Channel chan);
static void		DiscardInputQueued(ChannelState *statePtr,
			    int discardSavedBuffers);
static void		DiscardOutputQueued(ChannelState *chanPtr);
static int		DoRead(Channel *chanPtr, char *srcPtr, int slen);
static int		DoWrite(Channel *chanPtr, const char *src, int srcLen);
static int		DoReadChars(Channel *chan, Tcl_Obj *objPtr, int toRead,
			    int appendFlag);
static int		DoWriteChars(Channel *chan, const char *src, int len);
static int		FilterInputBytes(Channel *chanPtr,
			    GetsState *statePtr);
static int		FlushChannel(Tcl_Interp *interp, Channel *chanPtr,
			    int calledFromAsyncFlush);
static int		TclGetsObjBinary(Tcl_Channel chan, Tcl_Obj *objPtr);
static void		FreeBinaryEncoding(ClientData clientData);
static Tcl_HashTable *	GetChannelTable(Tcl_Interp *interp);
static int		GetInput(Channel *chanPtr);
static int		HaveVersion(const Tcl_ChannelType *typePtr,
			    Tcl_ChannelTypeVersion minimumVersion);
static void		PeekAhead(Channel *chanPtr, char **dstEndPtr,
			    GetsState *gsPtr);
static int		ReadBytes(ChannelState *statePtr, Tcl_Obj *objPtr,
			    int charsLeft, int *offsetPtr);
static int		ReadChars(ChannelState *statePtr, Tcl_Obj *objPtr,
			    int charsLeft, int *offsetPtr, int *factorPtr);
static void		RecycleBuffer(ChannelState *statePtr,
			    ChannelBuffer *bufPtr, int mustDiscard);
static int		StackSetBlockMode(Channel *chanPtr, int mode);
static int		SetBlockMode(Tcl_Interp *interp, Channel *chanPtr,
			    int mode);
static void		StopCopy(CopyState *csPtr);
static int		TranslateInputEOL(ChannelState *statePtr, char *dst,
			    const char *src, int *dstLenPtr, int *srcLenPtr);
static int		TranslateOutputEOL(ChannelState *statePtr, char *dst,
			    const char *src, int *dstLenPtr, int *srcLenPtr);
static void		UpdateInterest(Channel *chanPtr);
static int		WriteBytes(Channel *chanPtr, const char *src,
			    int srcLen);
static int		WriteChars(Channel *chanPtr, const char *src,
			    int srcLen);
static Tcl_Obj *	FixLevelCode(Tcl_Obj *msg);
static void		SpliceChannel(Tcl_Channel chan);
static void		CutChannel(Tcl_Channel chan);

/*
 * Simplifying helper macros. All may use their argument(s) multiple times.
 * The ANSI C "prototypes" for the macros are listed below, together with a
 * short description of what the macro does.
 *
 * --------------------------------------------------------------------------
 * int BytesLeft(ChannelBuffer *bufPtr)
 *
 *	Returns the number of bytes of data remaining in the buffer.
 *
 * int SpaceLeft(ChannelBuffer *bufPtr)
 *
 *	Returns the number of bytes of space remaining at the end of the
 *	buffer.
 *
 * int IsBufferReady(ChannelBuffer *bufPtr)
 *
 *	Returns whether a buffer has bytes available within it.
 *
 * int IsBufferEmpty(ChannelBuffer *bufPtr)
 *
 *	Returns whether a buffer is entirely empty. Note that this is not the
 *	inverse of the above operation; trying to merge the two seems to lead
 *	to occasional crashes...
 *
 * int IsBufferFull(ChannelBuffer *bufPtr)
 *
 *	Returns whether more data can be added to a buffer.
 *
 * int IsBufferOverflowing(ChannelBuffer *bufPtr)
 *
 *	Returns whether a buffer has more data in it than it should.
 *
 * char *InsertPoint(ChannelBuffer *bufPtr)
 *
 *	Returns a pointer to where characters should be added to the buffer.
 *
 * char *RemovePoint(ChannelBuffer *bufPtr)
 *
 *	Returns a pointer to where characters should be removed from the
 *	buffer.
 * --------------------------------------------------------------------------
 */

#define BytesLeft(bufPtr) ((bufPtr)->nextAdded - (bufPtr)->nextRemoved)

#define SpaceLeft(bufPtr) ((bufPtr)->bufLength - (bufPtr)->nextAdded)

#define IsBufferReady(bufPtr) ((bufPtr)->nextAdded > (bufPtr)->nextRemoved)

#define IsBufferEmpty(bufPtr) ((bufPtr)->nextAdded == (bufPtr)->nextRemoved)

#define IsBufferFull(bufPtr) ((bufPtr)->nextAdded >= (bufPtr)->bufLength)

#define IsBufferOverflowing(bufPtr) ((bufPtr)->nextAdded > (bufPtr)->bufLength)

#define InsertPoint(bufPtr) ((bufPtr)->buf + (bufPtr)->nextAdded)

#define RemovePoint(bufPtr) ((bufPtr)->buf + (bufPtr)->nextRemoved)

/*
 * For working with channel state flag bits.
 */

#define SetFlag(statePtr, flag)		((statePtr)->flags |= (flag))
#define ResetFlag(statePtr, flag)	((statePtr)->flags &= ~(flag))

/*
 * Macro for testing whether a string (in optionName, length len) matches a
 * value (prefix matching rules). Arguments are the minimum length to match
 * and the value to match against. (Can't use Tcl_GetIndexFromObj as this is
 * used in a situation where no objects are available.)
 */

#define HaveOpt(minLength, nameString) \
	((len > (minLength)) && (optionName[1] == (nameString)[1]) \
		&& (strncmp(optionName, (nameString), len) == 0))

/*
 * The ChannelObjType type.  We actually store the ChannelState structure
 * as that lives longest and we want to return the bottomChanPtr when
 * requested (consistent with Tcl_GetChannel).  The setFromAny and
 * updateString can be NULL as they should not be called.
 */

static void		DupChannelIntRep(Tcl_Obj *objPtr, Tcl_Obj *copyPtr);
static int		SetChannelFromAny(Tcl_Interp *interp, Tcl_Obj *objPtr);
static void		UpdateStringOfChannel(Tcl_Obj *objPtr);
static void		FreeChannelIntRep(Tcl_Obj *objPtr);

static Tcl_ObjType tclChannelType = {
    "channel",			/* name for this type */
    FreeChannelIntRep,		/* freeIntRepProc */
    DupChannelIntRep,		/* dupIntRepProc */
    NULL,			/* updateStringProc UpdateStringOfChannel */
    NULL			/* setFromAnyProc SetChannelFromAny */
};

#define GET_CHANNELSTATE(objPtr) \
    ((ChannelState *) (objPtr)->internalRep.otherValuePtr)
#define SET_CHANNELSTATE(objPtr, storePtr) \
    ((objPtr)->internalRep.otherValuePtr = (void *) (storePtr))

#define BUSY_STATE(st,fl) \
     ((((st)->csPtrR) && ((fl) & TCL_READABLE)) || \
      (((st)->csPtrW) && ((fl) & TCL_WRITABLE)))

/*
 *---------------------------------------------------------------------------
 *
 * TclInitIOSubsystem --
 *
 *	Initialize all resources used by this subsystem on a per-process
 *	basis.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Depends on the memory subsystems.
 *
 *---------------------------------------------------------------------------
 */

void
TclInitIOSubsystem(void)
{
    /*
     * By fetching thread local storage we take care of allocating it for each
     * thread.
     */

    (void) TCL_TSD_INIT(&dataKey);
}

/*
 *-------------------------------------------------------------------------
 *
 * TclFinalizeIOSubsystem --
 *
 *	Releases all resources used by this subsystem on a per-process basis.
 *	Closes all extant channels that have not already been closed because
 *	they were not owned by any interp.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Depends on encoding and memory subsystems.
 *
 *-------------------------------------------------------------------------
 */

	/* ARGSUSED */
void
TclFinalizeIOSubsystem(void)
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    Channel *chanPtr = NULL;	/* Iterates over open channels. */
    ChannelState *statePtr;	/* State of channel stack */
    int active = 1;		/* Flag == 1 while there's still work to do */

    /*
     * Walk all channel state structures known to this thread and close
     * corresponding channels.
     */

    while (active) {
	/*
	 * Iterate through the open channel list, and find the first channel
	 * that isn't dead. We start from the head of the list each time,
	 * because the close action on one channel can close others.
	 */

	active = 0;
	for (statePtr = tsdPtr->firstCSPtr;
		statePtr != NULL;
		statePtr = statePtr->nextCSPtr) {
	    chanPtr = statePtr->topChanPtr;
	    if (!(statePtr->flags & CHANNEL_DEAD)) {
		active = 1;
		break;
	    }
	}

	/*
	 * We've found a live channel. Close it.
	 */

	if (active) {
	    /*
	     * Set the channel back into blocking mode to ensure that we wait
	     * for all data to flush out.
	     */

	    (void) Tcl_SetChannelOption(NULL, (Tcl_Channel) chanPtr,
		    "-blocking", "on");

	    if ((chanPtr == (Channel *) tsdPtr->stdinChannel) ||
		    (chanPtr == (Channel *) tsdPtr->stdoutChannel) ||
		    (chanPtr == (Channel *) tsdPtr->stderrChannel)) {
		/*
		 * Decrement the refcount which was earlier artificially
		 * bumped up to keep the channel from being closed.
		 */

		statePtr->refCount--;
	    }

	    if (statePtr->refCount <= 0) {
		/*
		 * Close it only if the refcount indicates that the channel is
		 * not referenced from any interpreter. If it is, that
		 * interpreter will close the channel when it gets destroyed.
		 */

		(void) Tcl_Close(NULL, (Tcl_Channel) chanPtr);
	    } else {
		/*
		 * The refcount is greater than zero, so flush the channel.
		 */

		Tcl_Flush((Tcl_Channel) chanPtr);

		/*
		 * Call the device driver to actually close the underlying
		 * device for this channel.
		 */

		if (chanPtr->typePtr->closeProc != TCL_CLOSE2PROC) {
		    (chanPtr->typePtr->closeProc)(chanPtr->instanceData, NULL);
		} else {
		    (chanPtr->typePtr->close2Proc)(chanPtr->instanceData,
			    NULL, 0);
		}

		/*
		 * Finally, we clean up the fields in the channel data
		 * structure since all of them have been deleted already. We
		 * mark the channel with CHANNEL_DEAD to prevent any further
		 * IO operations on it.
		 */

		chanPtr->instanceData = NULL;
		SetFlag(statePtr, CHANNEL_DEAD);
	    }
	}
    }

    TclpFinalizeSockets();
    TclpFinalizePipes();
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetStdChannel --
 *
 *	This function is used to change the channels that are used for
 *	stdin/stdout/stderr in new interpreters.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_SetStdChannel(
    Tcl_Channel channel,
    int type)			/* One of TCL_STDIN, TCL_STDOUT, TCL_STDERR. */
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    switch (type) {
    case TCL_STDIN:
	tsdPtr->stdinInitialized = 1;
	tsdPtr->stdinChannel = channel;
	break;
    case TCL_STDOUT:
	tsdPtr->stdoutInitialized = 1;
	tsdPtr->stdoutChannel = channel;
	break;
    case TCL_STDERR:
	tsdPtr->stderrInitialized = 1;
	tsdPtr->stderrChannel = channel;
	break;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetStdChannel --
 *
 *	Returns the specified standard channel.
 *
 * Results:
 *	Returns the specified standard channel, or NULL.
 *
 * Side effects:
 *	May cause the creation of a standard channel and the underlying file.
 *
 *----------------------------------------------------------------------
 */

Tcl_Channel
Tcl_GetStdChannel(
    int type)			/* One of TCL_STDIN, TCL_STDOUT, TCL_STDERR. */
{
    Tcl_Channel channel = NULL;
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    /*
     * If the channels were not created yet, create them now and store them in
     * the static variables.
     */

    switch (type) {
    case TCL_STDIN:
	if (!tsdPtr->stdinInitialized) {
	    tsdPtr->stdinChannel = TclpGetDefaultStdChannel(TCL_STDIN);
	    tsdPtr->stdinInitialized = 1;

	    /*
	     * Artificially bump the refcount to ensure that the channel is
	     * only closed on exit.
	     *
	     * NOTE: Must only do this if stdinChannel is not NULL. It can be
	     * NULL in situations where Tcl is unable to connect to the
	     * standard input.
	     */

	    if (tsdPtr->stdinChannel != NULL) {
		Tcl_RegisterChannel(NULL, tsdPtr->stdinChannel);
	    }
	}
	channel = tsdPtr->stdinChannel;
	break;
    case TCL_STDOUT:
	if (!tsdPtr->stdoutInitialized) {
	    tsdPtr->stdoutChannel = TclpGetDefaultStdChannel(TCL_STDOUT);
	    tsdPtr->stdoutInitialized = 1;
	    if (tsdPtr->stdoutChannel != NULL) {
		Tcl_RegisterChannel(NULL, tsdPtr->stdoutChannel);
	    }
	}
	channel = tsdPtr->stdoutChannel;
	break;
    case TCL_STDERR:
	if (!tsdPtr->stderrInitialized) {
	    tsdPtr->stderrChannel = TclpGetDefaultStdChannel(TCL_STDERR);
	    tsdPtr->stderrInitialized = 1;
	    if (tsdPtr->stderrChannel != NULL) {
		Tcl_RegisterChannel(NULL, tsdPtr->stderrChannel);
	    }
	}
	channel = tsdPtr->stderrChannel;
	break;
    }
    return channel;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_CreateCloseHandler
 *
 *	Creates a close callback which will be called when the channel is
 *	closed.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Causes the callback to be called in the future when the channel will
 *	be closed.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_CreateCloseHandler(
    Tcl_Channel chan,		/* The channel for which to create the close
				 * callback. */
    Tcl_CloseProc *proc,	/* The callback routine to call when the
				 * channel will be closed. */
    ClientData clientData)	/* Arbitrary data to pass to the close
				 * callback. */
{
    ChannelState *statePtr;
    CloseCallback *cbPtr;

    statePtr = ((Channel *) chan)->state;

    cbPtr = (CloseCallback *) ckalloc(sizeof(CloseCallback));
    cbPtr->proc = proc;
    cbPtr->clientData = clientData;

    cbPtr->nextPtr = statePtr->closeCbPtr;
    statePtr->closeCbPtr = cbPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_DeleteCloseHandler --
 *
 *	Removes a callback that would have been called on closing the channel.
 *	If there is no matching callback then this function has no effect.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The callback will not be called in the future when the channel is
 *	eventually closed.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_DeleteCloseHandler(
    Tcl_Channel chan,		/* The channel for which to cancel the close
				 * callback. */
    Tcl_CloseProc *proc,	/* The procedure for the callback to
				 * remove. */
    ClientData clientData)	/* The callback data for the callback to
				 * remove. */
{
    ChannelState *statePtr;
    CloseCallback *cbPtr, *cbPrevPtr;

    statePtr = ((Channel *) chan)->state;
    for (cbPtr = statePtr->closeCbPtr, cbPrevPtr = NULL;
	    cbPtr != NULL; cbPtr = cbPtr->nextPtr) {
	if ((cbPtr->proc == proc) && (cbPtr->clientData == clientData)) {
	    if (cbPrevPtr == NULL) {
		statePtr->closeCbPtr = cbPtr->nextPtr;
	    }
	    ckfree((char *) cbPtr);
	    break;
	} else {
	    cbPrevPtr = cbPtr;
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * GetChannelTable --
 *
 *	Gets and potentially initializes the channel table for an interpreter.
 *	If it is initializing the table it also inserts channels for stdin,
 *	stdout and stderr if the interpreter is trusted.
 *
 * Results:
 *	A pointer to the hash table created, for use by the caller.
 *
 * Side effects:
 *	Initializes the channel table for an interpreter. May create channels
 *	for stdin, stdout and stderr.
 *
 *----------------------------------------------------------------------
 */

static Tcl_HashTable *
GetChannelTable(
    Tcl_Interp *interp)
{
    Tcl_HashTable *hTblPtr;	/* Hash table of channels. */
    Tcl_Channel stdinChan, stdoutChan, stderrChan;

    hTblPtr = Tcl_GetAssocData(interp, "tclIO", NULL);
    if (hTblPtr == NULL) {
	hTblPtr = (Tcl_HashTable *) ckalloc(sizeof(Tcl_HashTable));
	Tcl_InitHashTable(hTblPtr, TCL_STRING_KEYS);
	Tcl_SetAssocData(interp, "tclIO",
		(Tcl_InterpDeleteProc *) DeleteChannelTable, hTblPtr);

	/*
	 * If the interpreter is trusted (not "safe"), insert channels for
	 * stdin, stdout and stderr (possibly creating them in the process).
	 */

	if (Tcl_IsSafe(interp) == 0) {
	    stdinChan = Tcl_GetStdChannel(TCL_STDIN);
	    if (stdinChan != NULL) {
		Tcl_RegisterChannel(interp, stdinChan);
	    }
	    stdoutChan = Tcl_GetStdChannel(TCL_STDOUT);
	    if (stdoutChan != NULL) {
		Tcl_RegisterChannel(interp, stdoutChan);
	    }
	    stderrChan = Tcl_GetStdChannel(TCL_STDERR);
	    if (stderrChan != NULL) {
		Tcl_RegisterChannel(interp, stderrChan);
	    }
	}
    }
    return hTblPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * DeleteChannelTable --
 *
 *	Deletes the channel table for an interpreter, closing any open
 *	channels whose refcount reaches zero. This procedure is invoked when
 *	an interpreter is deleted, via the AssocData cleanup mechanism.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Deletes the hash table of channels. May close channels. May flush
 *	output on closed channels. Removes any channeEvent handlers that were
 *	registered in this interpreter.
 *
 *----------------------------------------------------------------------
 */

static void
DeleteChannelTable(
    ClientData clientData,	/* The per-interpreter data structure. */
    Tcl_Interp *interp)		/* The interpreter being deleted. */
{
    Tcl_HashTable *hTblPtr;	/* The hash table. */
    Tcl_HashSearch hSearch;	/* Search variable. */
    Tcl_HashEntry *hPtr;	/* Search variable. */
    Channel *chanPtr;		/* Channel being deleted. */
    ChannelState *statePtr;	/* State of Channel being deleted. */
    EventScriptRecord *sPtr, *prevPtr, *nextPtr;
				/* Variables to loop over all channel events
				 * registered, to delete the ones that refer
				 * to the interpreter being deleted. */

    /*
     * Delete all the registered channels - this will close channels whose
     * refcount reaches zero.
     */

    hTblPtr = clientData;
    for (hPtr = Tcl_FirstHashEntry(hTblPtr, &hSearch); hPtr != NULL;
	    hPtr = Tcl_FirstHashEntry(hTblPtr, &hSearch)) {
	chanPtr = Tcl_GetHashValue(hPtr);
	statePtr = chanPtr->state;

	/*
	 * Remove any fileevents registered in this interpreter.
	 */

	for (sPtr = statePtr->scriptRecordPtr, prevPtr = NULL;
		sPtr != NULL; sPtr = nextPtr) {
	    nextPtr = sPtr->nextPtr;
	    if (sPtr->interp == interp) {
		if (prevPtr == NULL) {
		    statePtr->scriptRecordPtr = nextPtr;
		} else {
		    prevPtr->nextPtr = nextPtr;
		}

		Tcl_DeleteChannelHandler((Tcl_Channel) chanPtr,
			TclChannelEventScriptInvoker, (ClientData) sPtr);

		TclDecrRefCount(sPtr->scriptPtr);
		ckfree((char *) sPtr);
	    } else {
		prevPtr = sPtr;
	    }
	}

	/*
	 * Cannot call Tcl_UnregisterChannel because that procedure calls
	 * Tcl_GetAssocData to get the channel table, which might already be
	 * inaccessible from the interpreter structure. Instead, we emulate
	 * the behavior of Tcl_UnregisterChannel directly here.
	 */

	Tcl_DeleteHashEntry(hPtr);
	SetFlag(statePtr, CHANNEL_TAINTED);
	statePtr->refCount--;
	if (statePtr->refCount <= 0) {
	    if (!(statePtr->flags & BG_FLUSH_SCHEDULED)) {
		(void) Tcl_Close(interp, (Tcl_Channel) chanPtr);
	    }
	}

    }
    Tcl_DeleteHashTable(hTblPtr);
    ckfree((char *) hTblPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * CheckForStdChannelsBeingClosed --
 *
 *	Perform special handling for standard channels being closed. When
 *	given a standard channel, if the refcount is now 1, it means that the
 *	last reference to the standard channel is being explicitly closed. Now
 *	bump the refcount artificially down to 0, to ensure the normal
 *	handling of channels being closed will occur. Also reset the static
 *	pointer to the channel to NULL, to avoid dangling references.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Manipulates the refcount on standard channels. May smash the global
 *	static pointer to a standard channel.
 *
 *----------------------------------------------------------------------
 */

static void
CheckForStdChannelsBeingClosed(
    Tcl_Channel chan)
{
    ChannelState *statePtr = ((Channel *) chan)->state;
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    if ((chan == tsdPtr->stdinChannel) && (tsdPtr->stdinInitialized)) {
	if (statePtr->refCount < 2) {
	    statePtr->refCount = 0;
	    tsdPtr->stdinChannel = NULL;
	    return;
	}
    } else if ((chan == tsdPtr->stdoutChannel)
	    && (tsdPtr->stdoutInitialized)) {
	if (statePtr->refCount < 2) {
	    statePtr->refCount = 0;
	    tsdPtr->stdoutChannel = NULL;
	    return;
	}
    } else if ((chan == tsdPtr->stderrChannel)
	    && (tsdPtr->stderrInitialized)) {
	if (statePtr->refCount < 2) {
	    statePtr->refCount = 0;
	    tsdPtr->stderrChannel = NULL;
	    return;
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_IsStandardChannel --
 *
 *	Test if the given channel is a standard channel. No attempt is made to
 *	check if the channel or the standard channels are initialized or
 *	otherwise valid.
 *
 * Results:
 *	Returns 1 if true, 0 if false.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_IsStandardChannel(
    Tcl_Channel chan)		/* Channel to check. */
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    if ((chan == tsdPtr->stdinChannel)
	    || (chan == tsdPtr->stdoutChannel)
	    || (chan == tsdPtr->stderrChannel)) {
	return 1;
    } else {
	return 0;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_RegisterChannel --
 *
 *	Adds an already-open channel to the channel table of an interpreter.
 *	If the interpreter passed as argument is NULL, it only increments the
 *	channel refCount.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	May increment the reference count of a channel.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_RegisterChannel(
    Tcl_Interp *interp,		/* Interpreter in which to add the channel. */
    Tcl_Channel chan)		/* The channel to add to this interpreter
				 * channel table. */
{
    Tcl_HashTable *hTblPtr;	/* Hash table of channels. */
    Tcl_HashEntry *hPtr;	/* Search variable. */
    int isNew;			/* Is the hash entry new or does it exist? */
    Channel *chanPtr;		/* The actual channel. */
    ChannelState *statePtr;	/* State of the actual channel. */

    /*
     * Always (un)register bottom-most channel in the stack. This makes
     * management of the channel list easier because no manipulation is
     * necessary during (un)stack operation.
     */

    chanPtr = ((Channel *) chan)->state->bottomChanPtr;
    statePtr = chanPtr->state;

    if (statePtr->channelName == NULL) {
	Tcl_Panic("Tcl_RegisterChannel: channel without name");
    }
    if (interp != NULL) {
	hTblPtr = GetChannelTable(interp);
	hPtr = Tcl_CreateHashEntry(hTblPtr, statePtr->channelName, &isNew);
	if (!isNew) {
	    if (chan == Tcl_GetHashValue(hPtr)) {
		return;
	    }

	    Tcl_Panic("Tcl_RegisterChannel: duplicate channel names");
	}
	Tcl_SetHashValue(hPtr, chanPtr);
    }
    statePtr->refCount++;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_UnregisterChannel --
 *
 *	Deletes the hash entry for a channel associated with an interpreter.
 *	If the interpreter given as argument is NULL, it only decrements the
 *	reference count. (This all happens in the Tcl_DetachChannel helper
 *	function).
 *
 *	Finally, if the reference count of the channel drops to zero, it is
 *	deleted.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	Calls Tcl_DetachChannel which deletes the hash entry for a channel
 *	associated with an interpreter.
 *
 *	May delete the channel, which can have a variety of consequences,
 *	especially if we are forced to close the channel.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_UnregisterChannel(
    Tcl_Interp *interp,		/* Interpreter in which channel is defined. */
    Tcl_Channel chan)		/* Channel to delete. */
{
    ChannelState *statePtr;	/* State of the real channel. */

    statePtr = ((Channel *) chan)->state->bottomChanPtr->state;

    if (statePtr->flags & CHANNEL_INCLOSE) {
	if (interp != NULL) {
	    Tcl_AppendResult(interp, "Illegal recursive call to close "
		    "through close-handler of channel", NULL);
	}
	return TCL_ERROR;
    }

    if (DetachChannel(interp, chan) != TCL_OK) {
	return TCL_OK;
    }

    statePtr = ((Channel *) chan)->state->bottomChanPtr->state;

    /*
     * Perform special handling for standard channels being closed. If the
     * refCount is now 1 it means that the last reference to the standard
     * channel is being explicitly closed, so bump the refCount down
     * artificially to 0. This will ensure that the channel is actually
     * closed, below. Also set the static pointer to NULL for the channel.
     */

    CheckForStdChannelsBeingClosed(chan);

    /*
     * If the refCount reached zero, close the actual channel.
     */

    if (statePtr->refCount <= 0) {
	/*
	 * Ensure that if there is another buffer, it gets flushed whether or
	 * not we are doing a background flush.
	 */

	if ((statePtr->curOutPtr != NULL) &&
		IsBufferReady(statePtr->curOutPtr)) {
	    SetFlag(statePtr, BUFFER_READY);
	}
	Tcl_Preserve((ClientData)statePtr);
	if (!(statePtr->flags & BG_FLUSH_SCHEDULED)) {
	    /*
	     * We don't want to re-enter Tcl_Close().
	     */

	    if (!(statePtr->flags & CHANNEL_CLOSED)) {
		if (Tcl_Close(interp, chan) != TCL_OK) {
		    SetFlag(statePtr, CHANNEL_CLOSED);
		    Tcl_Release((ClientData)statePtr);
		    return TCL_ERROR;
		}
	    }
	}
	SetFlag(statePtr, CHANNEL_CLOSED);
	Tcl_Release((ClientData)statePtr);
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_DetachChannel --
 *
 *	Deletes the hash entry for a channel associated with an interpreter.
 *	If the interpreter given as argument is NULL, it only decrements the
 *	reference count. Even if the ref count drops to zero, the channel is
 *	NOT closed or cleaned up. This allows a channel to be detached from an
 *	interpreter and left in the same state it was in when it was
 *	originally returned by 'Tcl_OpenFileChannel', for example.
 *
 *	This function cannot be used on the standard channels, and will return
 *	TCL_ERROR if that is attempted.
 *
 *	This function should only be necessary for special purposes in which
 *	you need to generate a pristine channel from one that has already been
 *	used. All ordinary purposes will almost always want to use
 *	Tcl_UnregisterChannel instead.
 *
 *	Provided the channel is not attached to any other interpreter, it can
 *	then be closed with Tcl_Close, rather than with Tcl_UnregisterChannel.
 *
 * Results:
 *	A standard Tcl result. If the channel is not currently registered with
 *	the given interpreter, TCL_ERROR is returned, otherwise TCL_OK.
 *	However no error messages are left in the interp's result.
 *
 * Side effects:
 *	Deletes the hash entry for a channel associated with an interpreter.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_DetachChannel(
    Tcl_Interp *interp,		/* Interpreter in which channel is defined. */
    Tcl_Channel chan)		/* Channel to delete. */
{
    if (Tcl_IsStandardChannel(chan)) {
	return TCL_ERROR;
    }

    return DetachChannel(interp, chan);
}

/*
 *----------------------------------------------------------------------
 *
 * DetachChannel --
 *
 *	Deletes the hash entry for a channel associated with an interpreter.
 *	If the interpreter given as argument is NULL, it only decrements the
 *	reference count. Even if the ref count drops to zero, the channel is
 *	NOT closed or cleaned up. This allows a channel to be detached from an
 *	interpreter and left in the same state it was in when it was
 *	originally returned by 'Tcl_OpenFileChannel', for example.
 *
 * Results:
 *	A standard Tcl result. If the channel is not currently registered with
 *	the given interpreter, TCL_ERROR is returned, otherwise TCL_OK.
 *	However no error messages are left in the interp's result.
 *
 * Side effects:
 *	Deletes the hash entry for a channel associated with an interpreter.
 *
 *----------------------------------------------------------------------
 */

static int
DetachChannel(
    Tcl_Interp *interp,		/* Interpreter in which channel is defined. */
    Tcl_Channel chan)		/* Channel to delete. */
{
    Tcl_HashTable *hTblPtr;	/* Hash table of channels. */
    Tcl_HashEntry *hPtr;	/* Search variable. */
    Channel *chanPtr;		/* The real IO channel. */
    ChannelState *statePtr;	/* State of the real channel. */

    /*
     * Always (un)register bottom-most channel in the stack. This makes
     * management of the channel list easier because no manipulation is
     * necessary during (un)stack operation.
     */

    chanPtr = ((Channel *) chan)->state->bottomChanPtr;
    statePtr = chanPtr->state;

    if (interp != NULL) {
	hTblPtr = Tcl_GetAssocData(interp, "tclIO", NULL);
	if (hTblPtr == NULL) {
	    return TCL_ERROR;
	}
	hPtr = Tcl_FindHashEntry(hTblPtr, statePtr->channelName);
	if (hPtr == NULL) {
	    return TCL_ERROR;
	}
	if ((Channel *) Tcl_GetHashValue(hPtr) != chanPtr) {
	    return TCL_ERROR;
	}
	Tcl_DeleteHashEntry(hPtr);
	SetFlag(statePtr, CHANNEL_TAINTED);

	/*
	 * Remove channel handlers that refer to this interpreter, so that
	 * they will not be present if the actual close is delayed and more
	 * events happen on the channel. This may occur if the channel is
	 * shared between several interpreters, or if the channel has async
	 * flushing active.
	 */

	CleanupChannelHandlers(interp, chanPtr);
    }

    statePtr->refCount--;

    return TCL_OK;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_GetChannel --
 *
 *	Finds an existing Tcl_Channel structure by name in a given
 *	interpreter. This function is public because it is used by
 *	channel-type-specific functions.
 *
 * Results:
 *	A Tcl_Channel or NULL on failure. If failed, interp's result object
 *	contains an error message. *modePtr is filled with the modes in which
 *	the channel was opened.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */

Tcl_Channel
Tcl_GetChannel(
    Tcl_Interp *interp,		/* Interpreter in which to find or create the
				 * channel. */
    const char *chanName,	/* The name of the channel. */
    int *modePtr)		/* Where to store the mode in which the
				 * channel was opened? Will contain an ORed
				 * combination of TCL_READABLE and
				 * TCL_WRITABLE, if non-NULL. */
{
    Channel *chanPtr;		/* The actual channel. */
    Tcl_HashTable *hTblPtr;	/* Hash table of channels. */
    Tcl_HashEntry *hPtr;	/* Search variable. */
    const char *name;		/* Translated name. */

    /*
     * Substitute "stdin", etc. Note that even though we immediately find the
     * channel using Tcl_GetStdChannel, we still need to look it up in the
     * specified interpreter to ensure that it is present in the channel
     * table. Otherwise, safe interpreters would always have access to the
     * standard channels.
     */

    name = chanName;
    if ((chanName[0] == 's') && (chanName[1] == 't')) {
	chanPtr = NULL;
	if (strcmp(chanName, "stdin") == 0) {
	    chanPtr = (Channel *) Tcl_GetStdChannel(TCL_STDIN);
	} else if (strcmp(chanName, "stdout") == 0) {
	    chanPtr = (Channel *) Tcl_GetStdChannel(TCL_STDOUT);
	} else if (strcmp(chanName, "stderr") == 0) {
	    chanPtr = (Channel *) Tcl_GetStdChannel(TCL_STDERR);
	}
	if (chanPtr != NULL) {
	    name = chanPtr->state->channelName;
	}
    }

    hTblPtr = GetChannelTable(interp);
    hPtr = Tcl_FindHashEntry(hTblPtr, name);
    if (hPtr == NULL) {
	Tcl_AppendResult(interp, "can not find channel named \"", chanName,
		"\"", NULL);
	Tcl_SetErrorCode(interp, "TCL", "LOOKUP", "CHANNEL", chanName, NULL);
	return NULL;
    }

    /*
     * Always return bottom-most channel in the stack. This one lives the
     * longest - other channels may go away unnoticed. The other APIs
     * compensate where necessary to retrieve the topmost channel again.
     */

    chanPtr = Tcl_GetHashValue(hPtr);
    chanPtr = chanPtr->state->bottomChanPtr;
    if (modePtr != NULL) {
	*modePtr = (chanPtr->state->flags & (TCL_READABLE|TCL_WRITABLE));
    }

    return (Tcl_Channel) chanPtr;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclGetChannelFromObj --
 *
 *	Finds an existing Tcl_Channel structure by name in a given
 *	interpreter. This function is public because it is used by
 *	channel-type-specific functions.
 *
 * Results:
 *	A Tcl_Channel or NULL on failure. If failed, interp's result object
 *	contains an error message. *modePtr is filled with the modes in which
 *	the channel was opened.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */

int
TclGetChannelFromObj(
    Tcl_Interp *interp,		/* Interpreter in which to find or create the
				 * channel. */
    Tcl_Obj *objPtr,
    Tcl_Channel *channelPtr,
    int *modePtr,		/* Where to store the mode in which the
				 * channel was opened? Will contain an ORed
				 * combination of TCL_READABLE and
				 * TCL_WRITABLE, if non-NULL. */
    int flags)
{
    ChannelState *statePtr;

    if (SetChannelFromAny(interp, objPtr) != TCL_OK) {
	return TCL_ERROR;
    }

    statePtr = GET_CHANNELSTATE(objPtr);
    *channelPtr = (Tcl_Channel) (statePtr->bottomChanPtr);

    if (modePtr != NULL) {
	*modePtr = (statePtr->flags & (TCL_READABLE|TCL_WRITABLE));
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_CreateChannel --
 *
 *	Creates a new entry in the hash table for a Tcl_Channel record.
 *
 * Results:
 *	Returns the new Tcl_Channel.
 *
 * Side effects:
 *	Creates a new Tcl_Channel instance and inserts it into the hash table.
 *
 *----------------------------------------------------------------------
 */

Tcl_Channel
Tcl_CreateChannel(
    Tcl_ChannelType *typePtr, /* The channel type record. */
    const char *chanName,	/* Name of channel to record. */
    ClientData instanceData,	/* Instance specific data. */
    int mask)			/* TCL_READABLE & TCL_WRITABLE to indicate if
				 * the channel is readable, writable. */
{
    Channel *chanPtr;		/* The channel structure newly created. */
    ChannelState *statePtr;	/* The stack-level independent state info for
				 * the channel. */
    const char *name;
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    /*
     * With the change of the Tcl_ChannelType structure to use a version in
     * 8.3.2+, we have to make sure that our assumption that the structure
     * remains a binary compatible size is true.
     *
     * If this assertion fails on some system, then it can be removed only if
     * the user recompiles code with older channel drivers in the new system
     * as well.
     */

    assert(sizeof(Tcl_ChannelTypeVersion) == sizeof(Tcl_DriverBlockModeProc*));

    /*
     * JH: We could subsequently memset these to 0 to avoid the numerous
     * assignments to 0/NULL below.
     */

    chanPtr = (Channel *) ckalloc(sizeof(Channel));
    statePtr = (ChannelState *) ckalloc(sizeof(ChannelState));
    chanPtr->state = statePtr;

    chanPtr->instanceData = instanceData;
    chanPtr->typePtr = typePtr;

    /*
     * Set all the bits that are part of the stack-independent state
     * information for the channel.
     */

    if (chanName != NULL) {
	char *tmp = ckalloc((unsigned) (strlen(chanName) + 1));

	statePtr->channelName = tmp;
	strcpy(tmp, chanName);
    } else {
	Tcl_Panic("Tcl_CreateChannel: NULL channel name");
    }

    statePtr->flags = mask;

    /*
     * Set the channel to system default encoding.
     *
     * Note the strange bit of protection taking place here. If the system
     * encoding name is reported back as "binary", something weird is
     * happening. Tcl provides no "binary" encoding, so someone else has
     * provided one. We ignore it so as not to interfere with the "magic"
     * interpretation that Tcl_Channels give to the "-encoding binary" option.
     */

    statePtr->encoding = NULL;
    name = Tcl_GetEncodingName(NULL);
    if (strcmp(name, "binary") != 0) {
	statePtr->encoding = Tcl_GetEncoding(NULL, name);
    }
    statePtr->inputEncodingState  = NULL;
    statePtr->inputEncodingFlags  = TCL_ENCODING_START;
    statePtr->outputEncodingState = NULL;
    statePtr->outputEncodingFlags = TCL_ENCODING_START;

    /*
     * Set the channel up initially in AUTO input translation mode to accept
     * "\n", "\r" and "\r\n". Output translation mode is set to a platform
     * specific default value. The eofChar is set to 0 for both input and
     * output, so that Tcl does not look for an in-file EOF indicator (e.g.
     * ^Z) and does not append an EOF indicator to files.
     */

    statePtr->inputTranslation	= TCL_TRANSLATE_AUTO;
    statePtr->outputTranslation	= TCL_PLATFORM_TRANSLATION;
    statePtr->inEofChar		= 0;
    statePtr->outEofChar	= 0;

    statePtr->unreportedError	= 0;
    statePtr->refCount		= 0;
    statePtr->closeCbPtr	= NULL;
    statePtr->curOutPtr		= NULL;
    statePtr->outQueueHead	= NULL;
    statePtr->outQueueTail	= NULL;
    statePtr->saveInBufPtr	= NULL;
    statePtr->inQueueHead	= NULL;
    statePtr->inQueueTail	= NULL;
    statePtr->chPtr		= NULL;
    statePtr->interestMask	= 0;
    statePtr->scriptRecordPtr	= NULL;
    statePtr->bufSize		= CHANNELBUFFER_DEFAULT_SIZE;
    statePtr->timer		= NULL;
    statePtr->csPtrR		= NULL;
    statePtr->csPtrW		= NULL;

    statePtr->outputStage	= NULL;
    if ((statePtr->encoding != NULL) && (statePtr->flags & TCL_WRITABLE)) {
	statePtr->outputStage = (char *)
		ckalloc((unsigned) (statePtr->bufSize + 2));
    }

    /*
     * As we are creating the channel, it is obviously the top for now.
     */

    statePtr->topChanPtr	= chanPtr;
    statePtr->bottomChanPtr	= chanPtr;
    chanPtr->downChanPtr	= NULL;
    chanPtr->upChanPtr		= NULL;
    chanPtr->inQueueHead	= NULL;
    chanPtr->inQueueTail	= NULL;

    /*
     * TIP #219, Tcl Channel Reflection API
     */

    statePtr->chanMsg		= NULL;
    statePtr->unreportedMsg	= NULL;

    /*
     * Link the channel into the list of all channels; create an on-exit
     * handler if there is not one already, to close off all the channels in
     * the list on exit.
     *
     * JH: Could call Tcl_SpliceChannel, but need to avoid NULL check.
     *
     * TIP #218.
     * AK: Just initialize the field to NULL before invoking Tcl_SpliceChannel
     *	   We need Tcl_SpliceChannel, for the threadAction calls. There is no
     *	   real reason to duplicate all of this.
     * NOTE: All drivers using thread actions now have to perform their TSD
     *	     manipulation only in their thread action proc. Doing it when
     *	     creating their instance structures will collide with the thread
     *	     action activity and lead to damaged lists.
     */

    statePtr->nextCSPtr = NULL;
    SpliceChannel((Tcl_Channel) chanPtr);

    /*
     * Install this channel in the first empty standard channel slot, if the
     * channel was previously closed explicitly.
     */

    if ((tsdPtr->stdinChannel == NULL) && (tsdPtr->stdinInitialized == 1)) {
	Tcl_SetStdChannel((Tcl_Channel) chanPtr, TCL_STDIN);
	Tcl_RegisterChannel(NULL, (Tcl_Channel) chanPtr);
    } else if ((tsdPtr->stdoutChannel == NULL) &&
	    (tsdPtr->stdoutInitialized == 1)) {
	Tcl_SetStdChannel((Tcl_Channel) chanPtr, TCL_STDOUT);
	Tcl_RegisterChannel(NULL, (Tcl_Channel) chanPtr);
    } else if ((tsdPtr->stderrChannel == NULL) &&
	    (tsdPtr->stderrInitialized == 1)) {
	Tcl_SetStdChannel((Tcl_Channel) chanPtr, TCL_STDERR);
	Tcl_RegisterChannel(NULL, (Tcl_Channel) chanPtr);
    }
    return (Tcl_Channel) chanPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_StackChannel --
 *
 *	Replaces an entry in the hash table for a Tcl_Channel record. The
 *	replacement is a new channel with same name, it supercedes the
 *	replaced channel. Input and output of the superceded channel is now
 *	going through the newly created channel and allows the arbitrary
 *	filtering/manipulation of the dataflow.
 *
 *	Andreas Kupries <a.kupries@westend.com>, 12/13/1998 "Trf-Patch for
 *	filtering channels"
 *
 * Results:
 *	Returns the new Tcl_Channel, which actually contains the saved
 *	information about prevChan.
 *
 * Side effects:
 *	A new channel structure is allocated and linked below the existing
 *	channel. The channel operations and client data of the existing
 *	channel are copied down to the newly created channel, and the current
 *	channel has its operations replaced by the new typePtr.
 *
 *----------------------------------------------------------------------
 */

Tcl_Channel
Tcl_StackChannel(
    Tcl_Interp *interp,		/* The interpreter we are working in */
    Tcl_ChannelType *typePtr,	/* The channel type record for the new
				 * channel. */
    ClientData instanceData,	/* Instance specific data for the new
				 * channel. */
    int mask,			/* TCL_READABLE & TCL_WRITABLE to indicate if
				 * the channel is readable, writable. */
    Tcl_Channel prevChan)	/* The channel structure to replace */
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    Channel *chanPtr, *prevChanPtr;
    ChannelState *statePtr;
    Tcl_DriverThreadActionProc *threadActionProc;

    /*
     * Find the given channel (prevChan) in the list of all channels. If we do
     * not find it, then it was never registered correctly.
     *
     * This operation should occur at the top of a channel stack.
     */

    statePtr = (ChannelState *) tsdPtr->firstCSPtr;
    prevChanPtr = ((Channel *) prevChan)->state->topChanPtr;

    while ((statePtr != NULL) && (statePtr->topChanPtr != prevChanPtr)) {
	statePtr = statePtr->nextCSPtr;
    }

    if (statePtr == NULL) {
	if (interp) {
	    Tcl_AppendResult(interp, "couldn't find state for channel \"",
		    Tcl_GetChannelName(prevChan), "\"", NULL);
	}
	return NULL;
    }

    /*
     * Here we check if the given "mask" matches the "flags" of the already
     * existing channel.
     *
     *	  | - | R | W | RW |
     *	--+---+---+---+----+	<=>  0 != (chan->mask & prevChan->mask)
     *	- |   |   |   |    |
     *	R |   | + |   | +  |	The superceding channel is allowed to restrict
     *	W |   |   | + | +  |	the capabilities of the superceded one!
     *	RW|   | + | + | +  |
     *	--+---+---+---+----+
     */

    if ((mask & (statePtr->flags & (TCL_READABLE | TCL_WRITABLE))) == 0) {
	if (interp) {
	    Tcl_AppendResult(interp,
		    "reading and writing both disallowed for channel \"",
		    Tcl_GetChannelName(prevChan), "\"", NULL);
	}
	return NULL;
    }

    /*
     * Flush the buffers. This ensures that any data still in them at this
     * time is not handled by the new transformation. Restrict this to
     * writable channels. Take care to hide a possible bg-copy in progress
     * from Tcl_Flush and the CheckForChannelErrors inside.
     */

    if ((mask & TCL_WRITABLE) != 0) {
        CopyState *csPtrR;
        CopyState *csPtrW;

        csPtrR           = statePtr->csPtrR;
	statePtr->csPtrR = NULL;

        csPtrW           = statePtr->csPtrW;
	statePtr->csPtrW = NULL;

	if (Tcl_Flush((Tcl_Channel) prevChanPtr) != TCL_OK) {
	    statePtr->csPtrR = csPtrR;
	    statePtr->csPtrW = csPtrW;
	    if (interp) {
		Tcl_AppendResult(interp, "could not flush channel \"",
			Tcl_GetChannelName(prevChan), "\"", NULL);
	    }
	    return NULL;
	}

	statePtr->csPtrR = csPtrR;
	statePtr->csPtrW = csPtrW;
    }

    /*
     * Discard any input in the buffers. They are not yet read by the user of
     * the channel, so they have to go through the new transformation before
     * reading. As the buffers contain the untransformed form their contents
     * are not only useless but actually distorts our view of the system.
     *
     * To preserve the information without having to read them again and to
     * avoid problems with the location in the channel (seeking might be
     * impossible) we move the buffers from the common state structure into
     * the channel itself. We use the buffers in the channel below the new
     * transformation to hold the data. In the future this allows us to write
     * transformations which pre-read data and push the unused part back when
     * they are going away.
     */

    if (((mask & TCL_READABLE) != 0) && (statePtr->inQueueHead != NULL)) {
	/*
	 * Remark: It is possible that the channel buffers contain data from
	 * some earlier push-backs.
	 */

	statePtr->inQueueTail->nextPtr = prevChanPtr->inQueueHead;
	prevChanPtr->inQueueHead = statePtr->inQueueHead;

	if (prevChanPtr->inQueueTail == NULL) {
	    prevChanPtr->inQueueTail = statePtr->inQueueTail;
	}

	statePtr->inQueueHead = NULL;
	statePtr->inQueueTail = NULL;
    }

    chanPtr = (Channel *) ckalloc(sizeof(Channel));

    /*
     * Save some of the current state into the new structure, reinitialize the
     * parts which will stay with the transformation.
     *
     * Remarks:
     */

    chanPtr->state		= statePtr;
    chanPtr->instanceData	= instanceData;
    chanPtr->typePtr		= typePtr;
    chanPtr->downChanPtr	= prevChanPtr;
    chanPtr->upChanPtr		= NULL;
    chanPtr->inQueueHead	= NULL;
    chanPtr->inQueueTail	= NULL;

    /*
     * Place new block at the head of a possibly existing list of previously
     * stacked channels.
     */

    prevChanPtr->upChanPtr	= chanPtr;
    statePtr->topChanPtr	= chanPtr;

    /*
     * TIP #218, Channel Thread Actions.
     *
     * We call the thread actions for the new channel directly. We _cannot_
     * use SpliceChannel, because the (thread-)global list of all channels
     * always contains the _ChannelState_ for a stack of channels, not the
     * individual channels. And SpliceChannel would not only call the thread
     * actions, but also add the shared ChannelState to this list a second
     * time, mangling it.
     */

    threadActionProc = Tcl_ChannelThreadActionProc(chanPtr->typePtr);
    if (threadActionProc != NULL) {
	(*threadActionProc)(chanPtr->instanceData, TCL_CHANNEL_THREAD_INSERT);
    }

    return (Tcl_Channel) chanPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_UnstackChannel --
 *
 *	Unstacks an entry in the hash table for a Tcl_Channel record. This is
 *	the reverse to 'Tcl_StackChannel'.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	If TCL_ERROR is returned, the posix error code will be set with
 *	Tcl_SetErrno. May leave a message in interp result as well.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_UnstackChannel(
    Tcl_Interp *interp,		/* The interpreter we are working in */
    Tcl_Channel chan)		/* The channel to unstack */
{
    Channel *chanPtr = (Channel *) chan;
    ChannelState *statePtr = chanPtr->state;
    int result = 0;
    Tcl_DriverThreadActionProc *threadActionProc;

    /*
     * This operation should occur at the top of a channel stack.
     */

    chanPtr = statePtr->topChanPtr;

    if (chanPtr->downChanPtr != NULL) {
	/*
	 * Instead of manipulating the per-thread / per-interp list/hashtable
	 * of registered channels we wind down the state of the transformation,
	 * and then restore the state of underlying channel into the old
	 * structure.
	 */

	Channel *downChanPtr = chanPtr->downChanPtr;

	/*
	 * Flush the buffers. This ensures that any data still in them at this
	 * time _is_ handled by the transformation we are unstacking right
	 * now. Restrict this to writable channels. Take care to hide a
	 * possible bg-copy in progress from Tcl_Flush and the
	 * CheckForChannelErrors inside.
	 */

	if (statePtr->flags & TCL_WRITABLE) {
	    CopyState *csPtrR;
	    CopyState *csPtrW;

	    csPtrR           = statePtr->csPtrR;
	    statePtr->csPtrR = NULL;

	    csPtrW           = statePtr->csPtrW;
	    statePtr->csPtrW = NULL;

	    if (Tcl_Flush((Tcl_Channel) chanPtr) != TCL_OK) {
		statePtr->csPtrR = csPtrR;
		statePtr->csPtrW = csPtrW;

		/*
		 * TIP #219, Tcl Channel Reflection API.
		 * Move error messages put by the driver into the chan/ip
		 * bypass area into the regular interpreter result. Fall back
		 * to the regular message if nothing was found in the
		 * bypasses.
		 */

		if (!TclChanCaughtErrorBypass(interp, chan) && interp) {
		    Tcl_AppendResult(interp, "could not flush channel \"",
			    Tcl_GetChannelName((Tcl_Channel) chanPtr), "\"",
			    NULL);
		}
		return TCL_ERROR;
	    }

	    statePtr->csPtrR  = csPtrR;
	    statePtr->csPtrW = csPtrW;
	}

	/*
	 * Anything in the input queue and the push-back buffers of the
	 * transformation going away is transformed data, but not yet read. As
	 * unstacking means that the caller does not want to see transformed
	 * data any more we have to discard these bytes. To avoid writing an
	 * analogue to 'DiscardInputQueued' we move the information in the
	 * push back buffers to the input queue and then call
	 * 'DiscardInputQueued' on that.
	 */

	if ((((statePtr->flags & TCL_READABLE) != 0)) &&
		((statePtr->inQueueHead != NULL) ||
		(chanPtr->inQueueHead != NULL))) {

	    if ((statePtr->inQueueHead != NULL) &&
		    (chanPtr->inQueueHead != NULL)) {
		statePtr->inQueueTail->nextPtr = chanPtr->inQueueHead;
		statePtr->inQueueTail = chanPtr->inQueueTail;
		statePtr->inQueueHead = statePtr->inQueueTail;

	    } else if (chanPtr->inQueueHead != NULL) {
		statePtr->inQueueHead = chanPtr->inQueueHead;
		statePtr->inQueueTail = chanPtr->inQueueTail;
	    }

	    chanPtr->inQueueHead = NULL;
	    chanPtr->inQueueTail = NULL;

	    DiscardInputQueued(statePtr, 0);
	}

	/*
	 * TIP #218, Channel Thread Actions.
	 *
	 * We call the thread actions for the new channel directly. We
	 * _cannot_ use CutChannel, because the (thread-)global list of all
	 * channels always contains the _ChannelState_ for a stack of
	 * channels, not the individual channels. And SpliceChannel would not
	 * only call the thread actions, but also remove the shared
	 * ChannelState from this list despite there being more channels for
	 * the state which are still active.
	 */

	threadActionProc = Tcl_ChannelThreadActionProc(chanPtr->typePtr);
	if (threadActionProc != NULL) {
	    (*threadActionProc)(chanPtr->instanceData,
		    TCL_CHANNEL_THREAD_REMOVE);
	}

	statePtr->topChanPtr = downChanPtr;
	downChanPtr->upChanPtr = NULL;

	/*
	 * Leave this link intact for closeproc
	 *  chanPtr->downChanPtr = NULL;
	 */

	/*
	 * Close and free the channel driver state.
	 */

	if (chanPtr->typePtr->closeProc != TCL_CLOSE2PROC) {
	    result = (chanPtr->typePtr->closeProc)(chanPtr->instanceData,
		    interp);
	} else {
	    result = (chanPtr->typePtr->close2Proc)(chanPtr->instanceData,
		    interp, 0);
	}

	chanPtr->typePtr = NULL;

	/*
	 * AK: Tcl_NotifyChannel may hold a reference to this block of memory
	 */

	Tcl_EventuallyFree(chanPtr, TCL_DYNAMIC);
	UpdateInterest(downChanPtr);

	if (result != 0) {
	    Tcl_SetErrno(result);

	    /*
	     * TIP #219, Tcl Channel Reflection API.
	     * Move error messages put by the driver into the chan/ip bypass
	     * area into the regular interpreter result.
	     */

	    TclChanCaughtErrorBypass(interp, chan);
	    return TCL_ERROR;
	}
    } else {
	/*
	 * This channel does not cover another one. Simply do a close, if
	 * necessary.
	 */

	if (statePtr->refCount <= 0) {
	    if (Tcl_Close(interp, chan) != TCL_OK) {
		/*
		 * TIP #219, Tcl Channel Reflection API.
		 * "TclChanCaughtErrorBypass" is not required here, it was
		 * done already by "Tcl_Close".
		 */

		return TCL_ERROR;
	    }
	}

	/*
	 * TIP #218, Channel Thread Actions.
	 * Not required in this branch, this is done by Tcl_Close. If
	 * Tcl_Close is not called then the ChannelState is still active in
	 * the thread and no action has to be taken either.
	 */
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetStackedChannel --
 *
 *	Determines whether the specified channel is stacked upon another.
 *
 * Results:
 *	NULL if the channel is not stacked upon another one, or a reference to
 *	the channel it is stacked upon. This reference can be used in queries,
 *	but modification is not allowed.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_Channel
Tcl_GetStackedChannel(
    Tcl_Channel chan)
{
    Channel *chanPtr = (Channel *) chan;
				/* The actual channel. */

    return (Tcl_Channel) chanPtr->downChanPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetTopChannel --
 *
 *	Returns the top channel of a channel stack.
 *
 * Results:
 *	NULL if the channel is not stacked upon another one, or a reference to
 *	the channel it is stacked upon. This reference can be used in queries,
 *	but modification is not allowed.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_Channel
Tcl_GetTopChannel(
    Tcl_Channel chan)
{
    Channel *chanPtr = (Channel *) chan;
				/* The actual channel. */

    return (Tcl_Channel) chanPtr->state->topChanPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetChannelInstanceData --
 *
 *	Returns the client data associated with a channel.
 *
 * Results:
 *	The client data.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

ClientData
Tcl_GetChannelInstanceData(
    Tcl_Channel chan)		/* Channel for which to return client data. */
{
    Channel *chanPtr = (Channel *) chan;
				/* The actual channel. */

    return chanPtr->instanceData;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetChannelThread --
 *
 *	Given a channel structure, returns the thread managing it. TIP #10
 *
 * Results:
 *	Returns the id of the thread managing the channel.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_ThreadId
Tcl_GetChannelThread(
    Tcl_Channel chan)		/* The channel to return the managing thread
				 * for. */
{
    Channel *chanPtr = (Channel *) chan;
				/* The actual channel. */

    return chanPtr->state->managingThread;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetChannelType --
 *
 *	Given a channel structure, returns the channel type structure.
 *
 * Results:
 *	Returns a pointer to the channel type structure.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_ChannelType *
Tcl_GetChannelType(
    Tcl_Channel chan)		/* The channel to return type for. */
{
    Channel *chanPtr = (Channel *) chan;
				/* The actual channel. */

    return chanPtr->typePtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetChannelMode --
 *
 *	Computes a mask indicating whether the channel is open for reading and
 *	writing.
 *
 * Results:
 *	An OR-ed combination of TCL_READABLE and TCL_WRITABLE.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_GetChannelMode(
    Tcl_Channel chan)		/* The channel for which the mode is being
				 * computed. */
{
    ChannelState *statePtr = ((Channel *) chan)->state;
				/* State of actual channel. */

    return (statePtr->flags & (TCL_READABLE | TCL_WRITABLE));
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetChannelName --
 *
 *	Returns the string identifying the channel name.
 *
 * Results:
 *	The string containing the channel name. This memory is owned by the
 *	generic layer and should not be modified by the caller.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

const char *
Tcl_GetChannelName(
    Tcl_Channel chan)		/* The channel for which to return the name. */
{
    ChannelState *statePtr;	/* State of actual channel. */

    statePtr = ((Channel *) chan)->state;
    return statePtr->channelName;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetChannelHandle --
 *
 *	Returns an OS handle associated with a channel.
 *
 * Results:
 *	Returns TCL_OK and places the handle in handlePtr, or returns
 *	TCL_ERROR on failure.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_GetChannelHandle(
    Tcl_Channel chan,		/* The channel to get file from. */
    int direction,		/* TCL_WRITABLE or TCL_READABLE. */
    ClientData *handlePtr)	/* Where to store handle */
{
    Channel *chanPtr;		/* The actual channel. */
    ClientData handle;
    int result;

    chanPtr = ((Channel *) chan)->state->bottomChanPtr;
    result = (chanPtr->typePtr->getHandleProc)(chanPtr->instanceData,
	    direction, &handle);
    if (handlePtr) {
	*handlePtr = handle;
    }
    return result;
}

/*
 *---------------------------------------------------------------------------
 *
 * AllocChannelBuffer --
 *
 *	A channel buffer has BUFFER_PADDING bytes extra at beginning to hold
 *	any bytes of a native-encoding character that got split by the end of
 *	the previous buffer and need to be moved to the beginning of the next
 *	buffer to make a contiguous string so it can be converted to UTF-8.
 *
 *	A channel buffer has BUFFER_PADDING bytes extra at the end to hold any
 *	bytes of a native-encoding character (generated from a UTF-8
 *	character) that overflow past the end of the buffer and need to be
 *	moved to the next buffer.
 *
 * Results:
 *	A newly allocated channel buffer.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */

static ChannelBuffer *
AllocChannelBuffer(
    int length)			/* Desired length of channel buffer. */
{
    ChannelBuffer *bufPtr;
    int n;

    n = length + CHANNELBUFFER_HEADER_SIZE + BUFFER_PADDING + BUFFER_PADDING;
    bufPtr = (ChannelBuffer *) ckalloc((unsigned) n);
    bufPtr->nextAdded	= BUFFER_PADDING;
    bufPtr->nextRemoved	= BUFFER_PADDING;
    bufPtr->bufLength	= length + BUFFER_PADDING;
    bufPtr->nextPtr	= NULL;
    return bufPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * RecycleBuffer --
 *
 *	Helper function to recycle input and output buffers. Ensures that two
 *	input buffers are saved (one in the input queue and another in the
 *	saveInBufPtr field) and that curOutPtr is set to a buffer. Only if
 *	these conditions are met is the buffer freed to the OS.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	May free a buffer to the OS.
 *
 *----------------------------------------------------------------------
 */

static void
RecycleBuffer(
    ChannelState *statePtr,	/* ChannelState in which to recycle buffers. */
    ChannelBuffer *bufPtr,	/* The buffer to recycle. */
    int mustDiscard)		/* If nonzero, free the buffer to the OS,
				 * always. */
{
    /*
     * Do we have to free the buffer to the OS?
     */

    if (mustDiscard) {
	ckfree((char *) bufPtr);
	return;
    }

    /*
     * Only save buffers which are at least as big as the requested buffersize
     * for the channel. This is to honor dynamic changes of the buffersize
     * made by the user.
     */

    if ((bufPtr->bufLength - BUFFER_PADDING) < statePtr->bufSize) {
	ckfree((char *) bufPtr);
	return;
    }

    /*
     * Only save buffers for the input queue if the channel is readable.
     */

    if (statePtr->flags & TCL_READABLE) {
	if (statePtr->inQueueHead == NULL) {
	    statePtr->inQueueHead = bufPtr;
	    statePtr->inQueueTail = bufPtr;
	    goto keepBuffer;
	}
	if (statePtr->saveInBufPtr == NULL) {
	    statePtr->saveInBufPtr = bufPtr;
	    goto keepBuffer;
	}
    }

    /*
     * Only save buffers for the output queue if the channel is writable.
     */

    if (statePtr->flags & TCL_WRITABLE) {
	if (statePtr->curOutPtr == NULL) {
	    statePtr->curOutPtr = bufPtr;
	    goto keepBuffer;
	}
    }

    /*
     * If we reached this code we return the buffer to the OS.
     */

    ckfree((char *) bufPtr);
    return;

  keepBuffer:
    bufPtr->nextRemoved = BUFFER_PADDING;
    bufPtr->nextAdded = BUFFER_PADDING;
    bufPtr->nextPtr = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * DiscardOutputQueued --
 *
 *	Discards all output queued in the output queue of a channel.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Recycles buffers.
 *
 *----------------------------------------------------------------------
 */

static void
DiscardOutputQueued(
    ChannelState *statePtr)	/* ChannelState for which to discard output. */
{
    ChannelBuffer *bufPtr;

    while (statePtr->outQueueHead != NULL) {
	bufPtr = statePtr->outQueueHead;
	statePtr->outQueueHead = bufPtr->nextPtr;
	RecycleBuffer(statePtr, bufPtr, 0);
    }
    statePtr->outQueueHead = NULL;
    statePtr->outQueueTail = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * CheckForDeadChannel --
 *
 *	This function checks is a given channel is Dead (a channel that has
 *	been closed but not yet deallocated.)
 *
 * Results:
 *	True (1) if channel is Dead, False (0) if channel is Ok
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

static int
CheckForDeadChannel(
    Tcl_Interp *interp,		/* For error reporting (can be NULL) */
    ChannelState *statePtr)	/* The channel state to check. */
{
    if (statePtr->flags & CHANNEL_DEAD) {
	Tcl_SetErrno(EINVAL);
	if (interp) {
	    Tcl_AppendResult(interp,
		    "unable to access channel: invalid channel", NULL);
	}
	return 1;
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * FlushChannel --
 *
 *	This function flushes as much of the queued output as is possible
 *	now. If calledFromAsyncFlush is nonzero, it is being called in an
 *	event handler to flush channel output asynchronously.
 *
 * Results:
 *	0 if successful, else the error code that was returned by the channel
 *	type operation. May leave a message in the interp result.
 *
 * Side effects:
 *	May produce output on a channel. May block indefinitely if the channel
 *	is synchronous. May schedule an async flush on the channel. May
 *	recycle memory for buffers in the output queue.
 *
 *----------------------------------------------------------------------
 */

static int
FlushChannel(
    Tcl_Interp *interp,		/* For error reporting during close. */
    Channel *chanPtr,		/* The channel to flush on. */
    int calledFromAsyncFlush)	/* If nonzero then we are being called from an
				 * asynchronous flush callback. */
{
    ChannelState *statePtr = chanPtr->state;
				/* State of the channel stack. */
    ChannelBuffer *bufPtr;	/* Iterates over buffered output queue. */
    int toWrite;		/* Amount of output data in current buffer
				 * available to be written. */
    int written;		/* Amount of output data actually written in
				 * current round. */
    int errorCode = 0;		/* Stores POSIX error codes from channel
				 * driver operations. */
    int wroteSome = 0;		/* Set to one if any data was written to the
				 * driver. */

    /*
     * Prevent writing on a dead channel -- a channel that has been closed but
     * not yet deallocated. This can occur if the exit handler for the channel
     * deallocation runs before all channels are deregistered in all
     * interpreters.
     */

    if (CheckForDeadChannel(interp, statePtr)) {
	return -1;
    }

    /*
     * Loop over the queued buffers and attempt to flush as much as possible
     * of the queued output to the channel.
     */

    while (1) {
	/*
	 * If the queue is empty and there is a ready current buffer, OR if
	 * the current buffer is full, then move the current buffer to the
	 * queue.
	 */

	if (((statePtr->curOutPtr != NULL) &&
		IsBufferFull(statePtr->curOutPtr))
		|| ((statePtr->flags & BUFFER_READY) &&
			(statePtr->outQueueHead == NULL))) {
	    ResetFlag(statePtr, BUFFER_READY);
	    statePtr->curOutPtr->nextPtr = NULL;
	    if (statePtr->outQueueHead == NULL) {
		statePtr->outQueueHead = statePtr->curOutPtr;
	    } else {
		statePtr->outQueueTail->nextPtr = statePtr->curOutPtr;
	    }
	    statePtr->outQueueTail = statePtr->curOutPtr;
	    statePtr->curOutPtr = NULL;
	}
	bufPtr = statePtr->outQueueHead;

	/*
	 * If we are not being called from an async flush and an async flush
	 * is active, we just return without producing any output.
	 */

	if ((!calledFromAsyncFlush) &&
		(statePtr->flags & BG_FLUSH_SCHEDULED)) {
	    return 0;
	}

	/*
	 * If the output queue is still empty, break out of the while loop.
	 */

	if (bufPtr == NULL) {
	    break;	/* Out of the "while (1)". */
	}

	/*
	 * Produce the output on the channel.
	 */

	toWrite = BytesLeft(bufPtr);
	written = (chanPtr->typePtr->outputProc)(chanPtr->instanceData,
		RemovePoint(bufPtr), toWrite, &errorCode);

	/*
	 * If the write failed completely attempt to start the asynchronous
	 * flush mechanism and break out of this loop - do not attempt to
	 * write any more output at this time.
	 */

	if (written < 0) {
	    /*
	     * If the last attempt to write was interrupted, simply retry.
	     */

	    if (errorCode == EINTR) {
		errorCode = 0;
		continue;
	    }

	    /*
	     * If the channel is non-blocking and we would have blocked, start
	     * a background flushing handler and break out of the loop.
	     */

	    if ((errorCode == EWOULDBLOCK) || (errorCode == EAGAIN)) {
		/*
		 * This used to check for CHANNEL_NONBLOCKING, and panic if
		 * the channel was blocking. However, it appears that setting
		 * stdin to -blocking 0 has some effect on the stdout when
		 * it's a tty channel (dup'ed underneath)
		 */

		if (!(statePtr->flags & BG_FLUSH_SCHEDULED)) {
		    SetFlag(statePtr, BG_FLUSH_SCHEDULED);
		    UpdateInterest(chanPtr);
		}
		errorCode = 0;
		break;
	    }

	    /*
	     * Decide whether to report the error upwards or defer it.
	     */

	    if (calledFromAsyncFlush) {
		/*
		 * TIP #219, Tcl Channel Reflection API.
		 * When defering the error copy a message from the bypass into
		 * the unreported area. Or discard it if the new error is to be
		 * ignored in favor of an earlier defered error.
		 */

		Tcl_Obj *msg = statePtr->chanMsg;

		if (statePtr->unreportedError == 0) {
		    statePtr->unreportedError = errorCode;
		    statePtr->unreportedMsg = msg;
		    if (msg != NULL) {
			Tcl_IncrRefCount(msg);
		    }
		} else {
		    /*
		     * An old unreported error is kept, and this error thrown
		     * away.
		     */

		    statePtr->chanMsg = NULL;
		    if (msg != NULL) {
			TclDecrRefCount(msg);
		    }
		}
	    } else {
		/*
		 * TIP #219, Tcl Channel Reflection API.
		 * Move error messages put by the driver into the chan bypass
		 * area into the regular interpreter result. Fall back to the
		 * regular message if nothing was found in the bypasses.
		 */

		Tcl_SetErrno(errorCode);
		if (interp != NULL && !TclChanCaughtErrorBypass(interp,
			(Tcl_Channel) chanPtr)) {
		    /*
		     * Casting away const here is safe because the
		     * TCL_VOLATILE flag guarantees const treatment of the
		     * Posix error string.
		     */

		    Tcl_SetResult(interp, (char *) Tcl_PosixError(interp),
			    TCL_VOLATILE);
		}

		/*
		 * An unreportable bypassed message is kept, for the caller of
		 * Tcl_Seek, Tcl_Write, etc.
		 */
	    }

	    /*
	     * When we get an error we throw away all the output currently
	     * queued.
	     */

	    DiscardOutputQueued(statePtr);
	    continue;
	} else {
	    wroteSome = 1;
	}

	bufPtr->nextRemoved += written;

	/*
	 * If this buffer is now empty, recycle it.
	 */

	if (IsBufferEmpty(bufPtr)) {
	    statePtr->outQueueHead = bufPtr->nextPtr;
	    if (statePtr->outQueueHead == NULL) {
		statePtr->outQueueTail = NULL;
	    }
	    RecycleBuffer(statePtr, bufPtr, 0);
	}
    }	/* Closes "while (1)". */

    /*
     * If we wrote some data while flushing in the background, we are done.
     * We can't finish the background flush until we run out of data and the
     * channel becomes writable again. This ensures that all of the pending
     * data has been flushed at the system level.
     */

    if (statePtr->flags & BG_FLUSH_SCHEDULED) {
	if (wroteSome) {
	    return errorCode;
	} else if (statePtr->outQueueHead == NULL) {
	    ResetFlag(statePtr, BG_FLUSH_SCHEDULED);
	    (chanPtr->typePtr->watchProc)(chanPtr->instanceData,
		    statePtr->interestMask);
	}
    }

    /*
     * If the channel is flagged as closed, delete it when the refCount drops
     * to zero, the output queue is empty and there is no output in the
     * current output buffer.
     */

    if ((statePtr->flags & CHANNEL_CLOSED) && (statePtr->refCount <= 0) &&
	    (statePtr->outQueueHead == NULL) &&
	    ((statePtr->curOutPtr == NULL) ||
	    IsBufferEmpty(statePtr->curOutPtr))) {
	return CloseChannel(interp, chanPtr, errorCode);
    }
    return errorCode;
}

/*
 *----------------------------------------------------------------------
 *
 * CloseChannel --
 *
 *	Utility procedure to close a channel and free associated resources.
 *
 *	If the channel was stacked, then the it will copy the necessary
 *	elements of the NEXT channel into the TOP channel, in essence
 *	unstacking the channel. The NEXT channel will then be freed.
 *
 *	If the channel was not stacked, then we will free all the bits for the
 *	TOP channel, including the data structure itself.
 *
 * Results:
 *	Error code from an unreported error or the driver close operation.
 *
 * Side effects:
 *	May close the actual channel, may free memory, may change the value of
 *	errno.
 *
 *----------------------------------------------------------------------
 */

static int
CloseChannel(
    Tcl_Interp *interp,		/* For error reporting. */
    Channel *chanPtr,		/* The channel to close. */
    int errorCode)		/* Status of operation so far. */
{
    int result = 0;		/* Of calling driver close operation. */
    ChannelState *statePtr;	/* State of the channel stack. */
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    if (chanPtr == NULL) {
	return result;
    }
    statePtr = chanPtr->state;

    /*
     * No more input can be consumed so discard any leftover input.
     */

    DiscardInputQueued(statePtr, 1);

    /*
     * Discard a leftover buffer in the current output buffer field.
     */

    if (statePtr->curOutPtr != NULL) {
	ckfree((char *) statePtr->curOutPtr);
	statePtr->curOutPtr = NULL;
    }

    /*
     * The caller guarantees that there are no more buffers queued for output.
     */

    if (statePtr->outQueueHead != NULL) {
	Tcl_Panic("TclFlush, closed channel: queued output left");
    }

    /*
     * If the EOF character is set in the channel, append that to the output
     * device.
     */

    if ((statePtr->outEofChar != 0) && (statePtr->flags & TCL_WRITABLE)) {
	int dummy;
	char c = (char) statePtr->outEofChar;

	(chanPtr->typePtr->outputProc)(chanPtr->instanceData, &c, 1, &dummy);
    }

    /*
     * TIP #219, Tcl Channel Reflection API.
     * Move a leftover error message in the channel bypass into the
     * interpreter bypass. Just clear it if there is no interpreter.
     */

    if (statePtr->chanMsg != NULL) {
	if (interp != NULL) {
	    Tcl_SetChannelErrorInterp(interp,statePtr->chanMsg);
	}
	TclDecrRefCount(statePtr->chanMsg);
	statePtr->chanMsg = NULL;
    }

    /*
     * Remove this channel from of the list of all channels.
     */

    CutChannel((Tcl_Channel) chanPtr);

    /*
     * Close and free the channel driver state.
     * This may leave a TIP #219 error message in the interp.
     */

    if (chanPtr->typePtr->closeProc != TCL_CLOSE2PROC) {
	result = (chanPtr->typePtr->closeProc)(chanPtr->instanceData, interp);
    } else {
	result = (chanPtr->typePtr->close2Proc)(chanPtr->instanceData,
		interp, 0);
    }

    /*
     * Some resources can be cleared only if the bottom channel in a stack is
     * closed. All the other channels in the stack are not allowed to remove.
     */

    if (chanPtr == statePtr->bottomChanPtr) {
	if (statePtr->channelName != NULL) {
	    ckfree((char *) statePtr->channelName);
	    statePtr->channelName = NULL;
	}

	Tcl_FreeEncoding(statePtr->encoding);
	if (statePtr->outputStage != NULL) {
	    ckfree((char *) statePtr->outputStage);
	    statePtr->outputStage = NULL;
	}
    }

    /*
     * If we are being called synchronously, report either any latent error on
     * the channel or the current error.
     */

    if (statePtr->unreportedError != 0) {
	errorCode = statePtr->unreportedError;

	/*
	 * TIP #219, Tcl Channel Reflection API.
	 * Move an error message found in the unreported area into the regular
	 * bypass (interp). This kills any message in the channel bypass area.
	 */

	if (statePtr->chanMsg != NULL) {
	    TclDecrRefCount(statePtr->chanMsg);
	    statePtr->chanMsg = NULL;
	}
	if (interp) {
	    Tcl_SetChannelErrorInterp(interp,statePtr->unreportedMsg);
	}
    }
    if (errorCode == 0) {
	errorCode = result;
	if (errorCode != 0) {
	    Tcl_SetErrno(errorCode);
	}
    }

    /*
     * Cancel any outstanding timer.
     */

    Tcl_DeleteTimerHandler(statePtr->timer);

    /*
     * Mark the channel as deleted by clearing the type structure.
     */

    if (chanPtr->downChanPtr != NULL) {
	Channel *downChanPtr = chanPtr->downChanPtr;

	statePtr->nextCSPtr = tsdPtr->firstCSPtr;
	tsdPtr->firstCSPtr = statePtr;

	statePtr->topChanPtr = downChanPtr;
	downChanPtr->upChanPtr = NULL;
	chanPtr->typePtr = NULL;

	Tcl_EventuallyFree(chanPtr, TCL_DYNAMIC);
	return Tcl_Close(interp, (Tcl_Channel) downChanPtr);
    }

    /*
     * There is only the TOP Channel, so we free the remaining pointers we
     * have and then ourselves. Since this is the last of the channels in the
     * stack, make sure to free the ChannelState structure associated with it.
     * We use Tcl_EventuallyFree to allow for any last references.
     */

    chanPtr->typePtr = NULL;

    Tcl_EventuallyFree(statePtr, TCL_DYNAMIC);
    Tcl_EventuallyFree(chanPtr, TCL_DYNAMIC);

    return errorCode;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_CutChannel --
 * CutChannel --
 *
 *	Removes a channel from the (thread-)global list of all channels (in
 *	that thread). This is actually the statePtr for the stack of channel.
 *
 * Results:
 *	Nothing.
 *
 * Side effects:
 *	Resets the field 'nextCSPtr' of the specified channel state to NULL.
 *
 * NOTE:
 *	The channel to cut out of the list must not be referenced in any
 *	interpreter. This is something this procedure cannot check (despite
 *	the refcount) because the caller usually wants fiddle with the channel
 *	(like transfering it to a different thread) and thus keeps the
 *	refcount artifically high to prevent its destruction.
 *
 *----------------------------------------------------------------------
 */

static void
CutChannel(
    Tcl_Channel chan)		/* The channel being removed. Must not be
				 * referenced in any interpreter. */
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    ChannelState *prevCSPtr;	/* Preceding channel state in list of all
				 * states - used to splice a channel out of
				 * the list on close. */
    ChannelState *statePtr = ((Channel *) chan)->state;
				/* State of the channel stack. */
    Tcl_DriverThreadActionProc *threadActionProc;

    /*
     * Remove this channel from of the list of all channels (in the current
     * thread).
     */

    if (tsdPtr->firstCSPtr && (statePtr == tsdPtr->firstCSPtr)) {
	tsdPtr->firstCSPtr = statePtr->nextCSPtr;
    } else {
	for (prevCSPtr = tsdPtr->firstCSPtr;
		prevCSPtr && (prevCSPtr->nextCSPtr != statePtr);
		prevCSPtr = prevCSPtr->nextCSPtr) {
	    /* Empty loop body. */
	}
	if (prevCSPtr == NULL) {
	    Tcl_Panic("FlushChannel: damaged channel list");
	}
	prevCSPtr->nextCSPtr = statePtr->nextCSPtr;
    }

    statePtr->nextCSPtr = NULL;

    /*
     * TIP #218, Channel Thread Actions
     */

    threadActionProc = Tcl_ChannelThreadActionProc(Tcl_GetChannelType(chan));
    if (threadActionProc != NULL) {
	(*threadActionProc)(Tcl_GetChannelInstanceData(chan),
		TCL_CHANNEL_THREAD_REMOVE);
    }
}

void
Tcl_CutChannel(
    Tcl_Channel chan)		/* The channel being added. Must not be
				 * referenced in any interpreter. */
{
    Channel *chanPtr = ((Channel *) chan)->state->bottomChanPtr;
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    ChannelState *prevCSPtr;	/* Preceding channel state in list of all
				 * states - used to splice a channel out of
				 * the list on close. */
    ChannelState *statePtr = chanPtr->state;
				/* State of the channel stack. */
    Tcl_DriverThreadActionProc *threadActionProc;

    /*
     * Remove this channel from of the list of all channels (in the current
     * thread).
     */

    if (tsdPtr->firstCSPtr && (statePtr == tsdPtr->firstCSPtr)) {
	tsdPtr->firstCSPtr = statePtr->nextCSPtr;
    } else {
	for (prevCSPtr = tsdPtr->firstCSPtr;
		prevCSPtr && (prevCSPtr->nextCSPtr != statePtr);
		prevCSPtr = prevCSPtr->nextCSPtr) {
	    /* Empty loop body. */
	}
	if (prevCSPtr == NULL) {
	    Tcl_Panic("FlushChannel: damaged channel list");
	}
	prevCSPtr->nextCSPtr = statePtr->nextCSPtr;
    }

    statePtr->nextCSPtr = NULL;

    /*
     * TIP #218, Channel Thread Actions
     * For all transformations and the base channel.
     */

    while (chanPtr) {
	threadActionProc = Tcl_ChannelThreadActionProc(chanPtr->typePtr);
	if (threadActionProc != NULL) {
	    (*threadActionProc)(chanPtr->instanceData,
		    TCL_CHANNEL_THREAD_REMOVE);
	}
	chanPtr= chanPtr->upChanPtr;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SpliceChannel --
 * SpliceChannel --
 *
 *	Adds a channel to the (thread-)global list of all channels (in that
 *	thread). Expects that the field 'nextChanPtr' in the channel is set to
 *	NULL.
 *
 * Results:
 *	Nothing.
 *
 * Side effects:
 *	Nothing.
 *
 * NOTE:
 *	The channel to splice into the list must not be referenced in any
 *	interpreter. This is something this procedure cannot check (despite
 *	the refcount) because the caller usually wants figgle with the channel
 *	(like transfering it to a different thread) and thus keeps the
 *	refcount artifically high to prevent its destruction.
 *
 *----------------------------------------------------------------------
 */

static void
SpliceChannel(
    Tcl_Channel chan)		/* The channel being added. Must not be
				 * referenced in any interpreter. */
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    ChannelState *statePtr = ((Channel *) chan)->state;
    Tcl_DriverThreadActionProc *threadActionProc;

    if (statePtr->nextCSPtr != NULL) {
	Tcl_Panic("SpliceChannel: trying to add channel used in different list");
    }

    statePtr->nextCSPtr = tsdPtr->firstCSPtr;
    tsdPtr->firstCSPtr = statePtr;

    /*
     * TIP #10. Mark the current thread as the new one managing this channel.
     *		Note: 'Tcl_GetCurrentThread' returns sensible values even for
     *		a non-threaded core.
     */

    statePtr->managingThread = Tcl_GetCurrentThread();

    /*
     * TIP #218, Channel Thread Actions
     */

    threadActionProc = Tcl_ChannelThreadActionProc(Tcl_GetChannelType(chan));
    if (threadActionProc != NULL) {
	(*threadActionProc) (Tcl_GetChannelInstanceData(chan),
		TCL_CHANNEL_THREAD_INSERT);
    }
}

void
Tcl_SpliceChannel(
    Tcl_Channel chan)		/* The channel being added. Must not be
				 * referenced in any interpreter. */
{
    Channel *chanPtr = ((Channel *) chan)->state->bottomChanPtr;
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    ChannelState *statePtr = chanPtr->state;
    Tcl_DriverThreadActionProc *threadActionProc;

    if (statePtr->nextCSPtr != NULL) {
	Tcl_Panic("SpliceChannel: trying to add channel used in different list");
    }

    statePtr->nextCSPtr = tsdPtr->firstCSPtr;
    tsdPtr->firstCSPtr = statePtr;

    /*
     * TIP #10. Mark the current thread as the new one managing this channel.
     *		Note: 'Tcl_GetCurrentThread' returns sensible values even for
     *		a non-threaded core.
     */

    statePtr->managingThread = Tcl_GetCurrentThread();

    /*
     * TIP #218, Channel Thread Actions
     * For all transformations and the base channel.
     */

    while (chanPtr) {
	threadActionProc = Tcl_ChannelThreadActionProc(chanPtr->typePtr);
	if (threadActionProc != NULL) {
	    (*threadActionProc)(chanPtr->instanceData,
		    TCL_CHANNEL_THREAD_INSERT);
	}
	chanPtr= chanPtr->upChanPtr;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_Close --
 *
 *	Closes a channel.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	Closes the channel if this is the last reference.
 *
 * NOTE:
 *	Tcl_Close removes the channel as far as the user is concerned.
 *	However, it may continue to exist for a while longer if it has a
 *	background flush scheduled. The device itself is eventually closed and
 *	the channel record removed, in CloseChannel, above.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
Tcl_Close(
    Tcl_Interp *interp,		/* Interpreter for errors. */
    Tcl_Channel chan)		/* The channel being closed. Must not be
				 * referenced in any interpreter. */
{
    CloseCallback *cbPtr;	/* Iterate over close callbacks for this
				 * channel. */
    Channel *chanPtr;		/* The real IO channel. */
    ChannelState *statePtr;	/* State of real IO channel. */
    int result;			/* Of calling FlushChannel. */
    int flushcode;

    if (chan == NULL) {
	return TCL_OK;
    }

    /*
     * Perform special handling for standard channels being closed. If the
     * refCount is now 1 it means that the last reference to the standard
     * channel is being explicitly closed, so bump the refCount down
     * artificially to 0. This will ensure that the channel is actually
     * closed, below. Also set the static pointer to NULL for the channel.
     */

    CheckForStdChannelsBeingClosed(chan);

    /*
     * This operation should occur at the top of a channel stack.
     */

    chanPtr = (Channel *) chan;
    statePtr = chanPtr->state;
    chanPtr = statePtr->topChanPtr;

    if (statePtr->refCount > 0) {
	Tcl_Panic("called Tcl_Close on channel with refCount > 0");
    }

    if (statePtr->flags & CHANNEL_INCLOSE) {
	if (interp) {
	    Tcl_AppendResult(interp, "Illegal recursive call to close "
		    "through close-handler of channel", NULL);
	}
	return TCL_ERROR;
    }
    SetFlag(statePtr, CHANNEL_INCLOSE);

    /*
     * When the channel has an escape sequence driven encoding such as
     * iso2022, the terminated escape sequence must write to the buffer.
     */

    if ((statePtr->encoding != NULL) && (statePtr->curOutPtr != NULL)
	    && (CheckChannelErrors(statePtr, TCL_WRITABLE) == 0)) {
	statePtr->outputEncodingFlags |= TCL_ENCODING_END;
	WriteChars(chanPtr, "", 0);

	/*
	 * TIP #219, Tcl Channel Reflection API.
	 * Move an error message found in the channel bypass into the
	 * interpreter bypass. Just clear it if there is no interpreter.
	 */

	if (statePtr->chanMsg != NULL) {
	    if (interp != NULL) {
		Tcl_SetChannelErrorInterp(interp,statePtr->chanMsg);
	    }
	    TclDecrRefCount(statePtr->chanMsg);
	    statePtr->chanMsg = NULL;
	}
    }

    Tcl_ClearChannelHandlers(chan);

    /*
     * Invoke the registered close callbacks and delete their records.
     */

    while (statePtr->closeCbPtr != NULL) {
	cbPtr = statePtr->closeCbPtr;
	statePtr->closeCbPtr = cbPtr->nextPtr;
	(cbPtr->proc)(cbPtr->clientData);
	ckfree((char *) cbPtr);
    }

    ResetFlag(statePtr, CHANNEL_INCLOSE);

    /*
     * Ensure that the last output buffer will be flushed.
     */

    if ((statePtr->curOutPtr != NULL) && IsBufferReady(statePtr->curOutPtr)) {
	SetFlag(statePtr, BUFFER_READY);
    }

    /*
     * If this channel supports it, close the read side, since we don't need
     * it anymore and this will help avoid deadlocks on some channel types.
     */

    if (chanPtr->typePtr->closeProc == TCL_CLOSE2PROC) {
	result = (chanPtr->typePtr->close2Proc)(chanPtr->instanceData, interp,
		TCL_CLOSE_READ);
    } else {
	result = 0;
    }

    /*
     * The call to FlushChannel will flush any queued output and invoke the
     * close function of the channel driver, or it will set up the channel to
     * be flushed and closed asynchronously.
     */

    SetFlag(statePtr, CHANNEL_CLOSED);

    flushcode = FlushChannel(interp, chanPtr, 0);

    /*
     * TIP #219.
     * Capture error messages put by the driver into the bypass area and put
     * them into the regular interpreter result.
     *
     * Notes: Due to the assertion of CHANNEL_CLOSED in the flags
     * FlushChannel() has called CloseChannel() and thus freed all the channel
     * structures. We must not try to access "chan" anymore, hence the NULL
     * argument in the call below. The only place which may still contain a
     * message is the interpreter itself, and "CloseChannel" made sure to lift
     * any channel message it generated into it.
     */

    if (TclChanCaughtErrorBypass(interp, NULL)) {
	result = EINVAL;
    }

    if ((flushcode != 0) || (result != 0)) {
	return TCL_ERROR;
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ClearChannelHandlers --
 *
 *	Removes all channel handlers and event scripts from the channel,
 *	cancels all background copies involving the channel and any interest
 *	in events.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	See above. Deallocates memory.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_ClearChannelHandlers(
    Tcl_Channel channel)
{
    ChannelHandler *chPtr, *chNext;	/* Iterate over channel handlers. */
    EventScriptRecord *ePtr, *eNextPtr;	/* Iterate over eventscript records. */
    Channel *chanPtr;			/* The real IO channel. */
    ChannelState *statePtr;		/* State of real IO channel. */
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    NextChannelHandler *nhPtr;

    /*
     * This operation should occur at the top of a channel stack.
     */

    chanPtr = (Channel *) channel;
    statePtr = chanPtr->state;
    chanPtr = statePtr->topChanPtr;

    /*
     * Cancel any outstanding timer.
     */

    Tcl_DeleteTimerHandler(statePtr->timer);

    /*
     * Remove any references to channel handlers for this channel that may be
     * about to be invoked.
     */

    for (nhPtr = tsdPtr->nestedHandlerPtr; nhPtr != NULL;
	    nhPtr = nhPtr->nestedHandlerPtr) {
	if (nhPtr->nextHandlerPtr &&
		(nhPtr->nextHandlerPtr->chanPtr == chanPtr)) {
	    nhPtr->nextHandlerPtr = NULL;
	}
    }

    /*
     * Remove all the channel handler records attached to the channel itself.
     */

    for (chPtr = statePtr->chPtr; chPtr != NULL; chPtr = chNext) {
	chNext = chPtr->nextPtr;
	ckfree((char *) chPtr);
    }
    statePtr->chPtr = NULL;

    /*
     * Cancel any pending copy operation.
     */

    StopCopy(statePtr->csPtrR);
    StopCopy(statePtr->csPtrW);

    /*
     * Must set the interest mask now to 0, otherwise infinite loops
     * will occur if Tcl_DoOneEvent is called before the channel is
     * finally deleted in FlushChannel. This can happen if the channel
     * has a background flush active.
     */

    statePtr->interestMask = 0;

    /*
     * Remove any EventScript records for this channel.
     */

    for (ePtr = statePtr->scriptRecordPtr; ePtr != NULL; ePtr = eNextPtr) {
	eNextPtr = ePtr->nextPtr;
	TclDecrRefCount(ePtr->scriptPtr);
	ckfree((char *) ePtr);
    }
    statePtr->scriptRecordPtr = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_Write --
 *
 *	Puts a sequence of bytes into an output buffer, may queue the buffer
 *	for output if it gets full, and also remembers whether the current
 *	buffer is ready e.g. if it contains a newline and we are in line
 *	buffering mode. Compensates stacking, i.e. will redirect the data from
 *	the specified channel to the topmost channel in a stack.
 *
 *	No encoding conversions are applied to the bytes being read.
 *
 * Results:
 *	The number of bytes written or -1 in case of error. If -1,
 *	Tcl_GetErrno will return the error code.
 *
 * Side effects:
 *	May buffer up output and may cause output to be produced on the
 *	channel.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_Write(
    Tcl_Channel chan,		/* The channel to buffer output for. */
    const char *src,		/* Data to queue in output buffer. */
    int srcLen)			/* Length of data in bytes, or < 0 for
				 * strlen(). */
{
    /*
     * Always use the topmost channel of the stack
     */

    Channel *chanPtr;
    ChannelState *statePtr;	/* State info for channel */

    statePtr = ((Channel *) chan)->state;
    chanPtr = statePtr->topChanPtr;

    if (CheckChannelErrors(statePtr, TCL_WRITABLE) != 0) {
	return -1;
    }

    if (srcLen < 0) {
	srcLen = strlen(src);
    }
    return DoWrite(chanPtr, src, srcLen);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_WriteRaw --
 *
 *	Puts a sequence of bytes into an output buffer, may queue the buffer
 *	for output if it gets full, and also remembers whether the current
 *	buffer is ready e.g. if it contains a newline and we are in line
 *	buffering mode. Writes directly to the driver of the channel, does not
 *	compensate for stacking.
 *
 *	No encoding conversions are applied to the bytes being read.
 *
 * Results:
 *	The number of bytes written or -1 in case of error. If -1,
 *	Tcl_GetErrno will return the error code.
 *
 * Side effects:
 *	May buffer up output and may cause output to be produced on the
 *	channel.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_WriteRaw(
    Tcl_Channel chan,		/* The channel to buffer output for. */
    const char *src,		/* Data to queue in output buffer. */
    int srcLen)			/* Length of data in bytes, or < 0 for
				 * strlen(). */
{
    Channel *chanPtr = ((Channel *) chan);
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    int errorCode, written;

    if (CheckChannelErrors(statePtr, TCL_WRITABLE | CHANNEL_RAW_MODE) != 0) {
	return -1;
    }

    if (srcLen < 0) {
	srcLen = strlen(src);
    }

    /*
     * Go immediately to the driver, do all the error handling by ourselves.
     * The code was stolen from 'FlushChannel'.
     */

    written = (chanPtr->typePtr->outputProc) (chanPtr->instanceData,
	    src, srcLen, &errorCode);

    if (written < 0) {
	Tcl_SetErrno(errorCode);
    }

    return written;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_WriteChars --
 *
 *	Takes a sequence of UTF-8 characters and converts them for output
 *	using the channel's current encoding, may queue the buffer for output
 *	if it gets full, and also remembers whether the current buffer is
 *	ready e.g. if it contains a newline and we are in line buffering
 *	mode. Compensates stacking, i.e. will redirect the data from the
 *	specified channel to the topmost channel in a stack.
 *
 * Results:
 *	The number of bytes written or -1 in case of error. If -1,
 *	Tcl_GetErrno will return the error code.
 *
 * Side effects:
 *	May buffer up output and may cause output to be produced on the
 *	channel.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_WriteChars(
    Tcl_Channel chan,		/* The channel to buffer output for. */
    const char *src,		/* UTF-8 characters to queue in output
				 * buffer. */
    int len)			/* Length of string in bytes, or < 0 for
				 * strlen(). */
{
    ChannelState *statePtr;	/* State info for channel */

    statePtr = ((Channel *) chan)->state;

    if (CheckChannelErrors(statePtr, TCL_WRITABLE) != 0) {
	return -1;
    }

    return DoWriteChars((Channel *) chan, src, len);
}

/*
 *---------------------------------------------------------------------------
 *
 * DoWriteChars --
 *
 *	Takes a sequence of UTF-8 characters and converts them for output
 *	using the channel's current encoding, may queue the buffer for output
 *	if it gets full, and also remembers whether the current buffer is
 *	ready e.g. if it contains a newline and we are in line buffering mode.
 *	Compensates stacking, i.e. will redirect the data from the specified
 *	channel to the topmost channel in a stack.
 *
 * Results:
 *	The number of bytes written or -1 in case of error. If -1,
 *	Tcl_GetErrno will return the error code.
 *
 * Side effects:
 *	May buffer up output and may cause output to be produced on the
 *	channel.
 *
 *----------------------------------------------------------------------
 */

static int
DoWriteChars(
    Channel *chanPtr,		/* The channel to buffer output for. */
    const char *src,		/* UTF-8 characters to queue in output
				 * buffer. */
    int len)			/* Length of string in bytes, or < 0 for
				 * strlen(). */
{
    /*
     * Always use the topmost channel of the stack
     */

    ChannelState *statePtr;	/* State info for channel */

    statePtr = chanPtr->state;
    chanPtr = statePtr->topChanPtr;

    if (len < 0) {
	len = strlen(src);
    }
    if (statePtr->encoding == NULL) {
	/*
	 * Inefficient way to convert UTF-8 to byte-array, but the code
	 * parallels the way it is done for objects.
	 * Special case for 1-byte (used by eg [puts] for the \n) could
	 * be extended to more efficient translation of the src string.
	 */

 	int result;

	if ((len == 1) && (UCHAR(*src) < 0xC0)) {
	    result = WriteBytes(chanPtr, src, len);
	} else {
	    Tcl_Obj *objPtr = Tcl_NewStringObj(src, len);
	    src = (char *) Tcl_GetByteArrayFromObj(objPtr, &len);
	    result = WriteBytes(chanPtr, src, len);
	    TclDecrRefCount(objPtr);
	}
	return result;
    }
    return WriteChars(chanPtr, src, len);
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_WriteObj --
 *
 *	Takes the Tcl object and queues its contents for output. If the
 *	encoding of the channel is NULL, takes the byte-array representation
 *	of the object and queues those bytes for output. Otherwise, takes the
 *	characters in the UTF-8 (string) representation of the object and
 *	converts them for output using the channel's current encoding. May
 *	flush internal buffers to output if one becomes full or is ready for
 *	some other reason, e.g. if it contains a newline and the channel is in
 *	line buffering mode.
 *
 * Results:
 *	The number of bytes written or -1 in case of error. If -1,
 *	Tcl_GetErrno() will return the error code.
 *
 * Side effects:
 *	May buffer up output and may cause output to be produced on the
 *	channel.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_WriteObj(
    Tcl_Channel chan,		/* The channel to buffer output for. */
    Tcl_Obj *objPtr)		/* The object to write. */
{
    /*
     * Always use the topmost channel of the stack
     */

    Channel *chanPtr;
    ChannelState *statePtr;	/* State info for channel */
    char *src;
    int srcLen;

    statePtr = ((Channel *) chan)->state;
    chanPtr = statePtr->topChanPtr;

    if (CheckChannelErrors(statePtr, TCL_WRITABLE) != 0) {
	return -1;
    }
    if (statePtr->encoding == NULL) {
	src = (char *) Tcl_GetByteArrayFromObj(objPtr, &srcLen);
	return WriteBytes(chanPtr, src, srcLen);
    } else {
	src = TclGetStringFromObj(objPtr, &srcLen);
	return WriteChars(chanPtr, src, srcLen);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * WriteBytes --
 *
 *	Write a sequence of bytes into an output buffer, may queue the buffer
 *	for output if it gets full, and also remembers whether the current
 *	buffer is ready e.g. if it contains a newline and we are in line
 *	buffering mode.
 *
 * Results:
 *	The number of bytes written or -1 in case of error. If -1,
 *	Tcl_GetErrno will return the error code.
 *
 * Side effects:
 *	May buffer up output and may cause output to be produced on the
 *	channel.
 *
 *----------------------------------------------------------------------
 */

static int
WriteBytes(
    Channel *chanPtr,		/* The channel to buffer output for. */
    const char *src,		/* Bytes to write. */
    int srcLen)			/* Number of bytes to write. */
{
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    ChannelBuffer *bufPtr;
    char *dst;
    int dstMax, sawLF, savedLF, total, dstLen, toWrite, translate;

    total = 0;
    sawLF = 0;
    savedLF = 0;
    translate = (statePtr->flags & CHANNEL_LINEBUFFERED)
	|| (statePtr->outputTranslation != TCL_TRANSLATE_LF);

    /*
     * Loop over all bytes in src, storing them in output buffer with proper
     * EOL translation.
     */

    while (srcLen + savedLF > 0) {
	bufPtr = statePtr->curOutPtr;
	if (bufPtr == NULL) {
	    bufPtr = AllocChannelBuffer(statePtr->bufSize);
	    statePtr->curOutPtr = bufPtr;
	}
	dst = InsertPoint(bufPtr);
	dstMax = SpaceLeft(bufPtr);
	dstLen = dstMax;

	toWrite = dstLen;
	if (toWrite > srcLen) {
	    toWrite = srcLen;
	}

	if (translate) {
	    if (savedLF) {
		/*
		 * A '\n' was left over from last call to TranslateOutputEOL()
		 * and we need to store it in this buffer. If the channel is
		 * line-based, we will need to flush it.
		 */

		*dst++ = '\n';
		dstLen--;
		sawLF++;
	    }
	    if (TranslateOutputEOL(statePtr, dst, src, &dstLen, &toWrite)) {
		sawLF++;
	    }
	    dstLen += savedLF;
	    savedLF = 0;
	    if (dstLen > dstMax) {
		savedLF = 1;
		dstLen = dstMax;
	    }
	} else {
	    memcpy(dst, src, toWrite);
	    dstLen = toWrite;
	}

	bufPtr->nextAdded += dstLen;
	if (CheckFlush(chanPtr, bufPtr, sawLF) != 0) {
	    return -1;
	}
	total += dstLen;
	src += toWrite;
	srcLen -= toWrite;
	sawLF = 0;
    }
    return total;
}

/*
 *----------------------------------------------------------------------
 *
 * WriteChars --
 *
 *	Convert UTF-8 bytes to the channel's external encoding and write the
 *	produced bytes into an output buffer, may queue the buffer for output
 *	if it gets full, and also remembers whether the current buffer is
 *	ready e.g. if it contains a newline and we are in line buffering mode.
 *
 * Results:
 *	The number of bytes written or -1 in case of error. If -1,
 *	Tcl_GetErrno will return the error code.
 *
 * Side effects:
 *	May buffer up output and may cause output to be produced on the
 *	channel.
 *
 *----------------------------------------------------------------------
 */

static int
WriteChars(
    Channel *chanPtr,		/* The channel to buffer output for. */
    const char *src,		/* UTF-8 string to write. */
    int srcLen)			/* Length of UTF-8 string in bytes. */
{
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    ChannelBuffer *bufPtr;
    char *dst, *stage;
    int saved, savedLF, sawLF, total, dstLen, stageMax, dstWrote;
    int stageLen, toWrite, stageRead, endEncoding, result;
    int consumedSomething, translate;
    Tcl_Encoding encoding;
    char safe[BUFFER_PADDING];

    total = 0;
    sawLF = 0;
    savedLF = 0;
    saved = 0;
    encoding = statePtr->encoding;

    /*
     * Write the terminated escape sequence even if srcLen is 0.
     */

    endEncoding = ((statePtr->outputEncodingFlags & TCL_ENCODING_END) != 0);

    translate = (statePtr->flags & CHANNEL_LINEBUFFERED)
	|| (statePtr->outputTranslation != TCL_TRANSLATE_LF);

    /*
     * Loop over all UTF-8 characters in src, storing them in staging buffer
     * with proper EOL translation.
     */

    consumedSomething = 1;
    while (consumedSomething && (srcLen + savedLF + endEncoding > 0)) {
	consumedSomething = 0;
	stage = statePtr->outputStage;
	stageMax = statePtr->bufSize;
	stageLen = stageMax;

	toWrite = stageLen;
	if (toWrite > srcLen) {
	    toWrite = srcLen;
	}

	if (translate) {
	    if (savedLF) {
		/*
		 * A '\n' was left over from last call to TranslateOutputEOL()
		 * and we need to store it in the staging buffer. If the channel
		 * is line-based, we will need to flush the output buffer (after
		 * translating the staging buffer).
		 */

		*stage++ = '\n';
		stageLen--;
		sawLF++;
	    }
	    if (TranslateOutputEOL(statePtr, stage, src, &stageLen, &toWrite)) {
		sawLF++;
	    }

	    stage -= savedLF;
	    stageLen += savedLF;
	    savedLF = 0;

	    if (stageLen > stageMax) {
		savedLF = 1;
		stageLen = stageMax;
	    }
	} else {
	    memcpy(stage, src, toWrite);
	    stageLen = toWrite;
	}
	src += toWrite;
	srcLen -= toWrite;

	/*
	 * Loop over all UTF-8 characters in staging buffer, converting them
	 * to external encoding, storing them in output buffer.
	 */

	while (stageLen + saved + endEncoding > 0) {
	    bufPtr = statePtr->curOutPtr;
	    if (bufPtr == NULL) {
		bufPtr = AllocChannelBuffer(statePtr->bufSize);
		statePtr->curOutPtr = bufPtr;
	    }
	    dst = InsertPoint(bufPtr);
	    dstLen = SpaceLeft(bufPtr);

	    if (saved != 0) {
		/*
		 * Here's some translated bytes left over from the last buffer
		 * that we need to stick at the beginning of this buffer.
		 */

		memcpy(dst, safe, (size_t) saved);
		bufPtr->nextAdded += saved;
		dst += saved;
		dstLen -= saved;
		saved = 0;
	    }

	    result = Tcl_UtfToExternal(NULL, encoding, stage, stageLen,
		    statePtr->outputEncodingFlags,
		    &statePtr->outputEncodingState, dst,
		    dstLen + BUFFER_PADDING, &stageRead, &dstWrote, NULL);

	    /*
	     * Fix for SF #506297, reported by Martin Forssen
	     * <ruric@users.sourceforge.net>.
	     *
	     * The encoding chosen in the script exposing the bug writes out
	     * three intro characters when TCL_ENCODING_START is set, but does
	     * not consume any input as TCL_ENCODING_END is cleared. As some
	     * output was generated the enclosing loop calls UtfToExternal
	     * again, again with START set. Three more characters in the out
	     * and still no use of input ... To break this infinite loop we
	     * remove TCL_ENCODING_START from the set of flags after the first
	     * call (no condition is required, the later calls remove an unset
	     * flag, which is a no-op). This causes the subsequent calls to
	     * UtfToExternal to consume and convert the actual input.
	     */

	    statePtr->outputEncodingFlags &= ~TCL_ENCODING_START;

	    /*
	     * The following code must be executed only when result is not 0.
	     */

	    if ((result != 0) && (stageRead + dstWrote == 0)) {
		/*
		 * We have an incomplete UTF-8 character at the end of the
		 * staging buffer. It will get moved to the beginning of the
		 * staging buffer followed by more bytes from src.
		 */

		src -= stageLen;
		srcLen += stageLen;
		stageLen = 0;
		savedLF = 0;
		break;
	    }
	    bufPtr->nextAdded += dstWrote;
	    if (IsBufferOverflowing(bufPtr)) {
		/*
		 * When translating from UTF-8 to external encoding, we
		 * allowed the translation to produce a character that crossed
		 * the end of the output buffer, so that we would get a
		 * completely full buffer before flushing it. The extra bytes
		 * will be moved to the beginning of the next buffer.
		 */

		saved = -SpaceLeft(bufPtr);
		memcpy(safe, dst + dstLen, (size_t) saved);
		bufPtr->nextAdded = bufPtr->bufLength;
	    }
	    if (CheckFlush(chanPtr, bufPtr, sawLF) != 0) {
		return -1;
	    }

	    total += dstWrote;
	    stage += stageRead;
	    stageLen -= stageRead;
	    sawLF = 0;

	    consumedSomething = 1;

	    /*
	     * If all translated characters are written to the buffer,
	     * endEncoding is set to 0 because the escape sequence may be
	     * output.
	     */

	    if ((stageLen + saved == 0) && (result == 0)) {
		endEncoding = 0;
	    }
	}
    }

    /*
     * If nothing was written and it happened because there was no progress in
     * the UTF conversion, we throw an error.
     */

    if (!consumedSomething && (total == 0)) {
	Tcl_SetErrno(EINVAL);
	return -1;
    }
    return total;
}

/*
 *---------------------------------------------------------------------------
 *
 * TranslateOutputEOL --
 *
 *	Helper function for WriteBytes() and WriteChars(). Converts the '\n'
 *	characters in the source buffer into the appropriate EOL form
 *	specified by the output translation mode.
 *
 *	EOL translation stops either when the source buffer is empty or the
 *	output buffer is full.
 *
 *	When converting to CRLF mode and there is only 1 byte left in the
 *	output buffer, this routine stores the '\r' in the last byte and then
 *	stores the '\n' in the byte just past the end of the buffer. The
 *	caller is responsible for passing in a buffer that is large enough to
 *	hold the extra byte.
 *
 * Results:
 *	The return value is 1 if a '\n' was translated from the source buffer,
 *	or 0 otherwise -- this can be used by the caller to decide to flush a
 *	line-based channel even though the channel buffer is not full.
 *
 *	*dstLenPtr is filled with how many bytes of the output buffer were
 *	used. As mentioned above, this can be one more that the output
 *	buffer's specified length if a CRLF was stored.
 *
 *	*srcLenPtr is filled with how many bytes of the source buffer were
 *	consumed.
 *
 * Side effects:
 *	It may be obvious, but bears mentioning that when converting in CRLF
 *	mode (which requires two bytes of storage in the output buffer), the
 *	number of bytes consumed from the source buffer will be less than the
 *	number of bytes stored in the output buffer.
 *
 *---------------------------------------------------------------------------
 */

static int
TranslateOutputEOL(
    ChannelState *statePtr,	/* Channel being read, for translation and
				 * buffering modes. */
    char *dst,			/* Output buffer filled with UTF-8 chars by
				 * applying appropriate EOL translation to
				 * source characters. */
    const char *src,		/* Source UTF-8 characters. */
    int *dstLenPtr,		/* On entry, the maximum length of output
				 * buffer in bytes. On exit, the number of
				 * bytes actually used in output buffer. */
    int *srcLenPtr)		/* On entry, the length of source buffer. On
				 * exit, the number of bytes read from the
				 * source buffer. */
{
    char *dstEnd;
    int srcLen, newlineFound;

    newlineFound = 0;
    srcLen = *srcLenPtr;

    switch (statePtr->outputTranslation) {
    case TCL_TRANSLATE_LF:
	for (dstEnd = dst + srcLen; dst < dstEnd; ) {
	    if (*src == '\n') {
		newlineFound = 1;
	    }
	    *dst++ = *src++;
	}
	*dstLenPtr = srcLen;
	break;
    case TCL_TRANSLATE_CR:
	for (dstEnd = dst + srcLen; dst < dstEnd;) {
	    if (*src == '\n') {
		*dst++ = '\r';
		newlineFound = 1;
		src++;
	    } else {
		*dst++ = *src++;
	    }
	}
	*dstLenPtr = srcLen;
	break;
    case TCL_TRANSLATE_CRLF: {
	/*
	 * Since this causes the number of bytes to grow, we start off trying
	 * to put 'srcLen' bytes into the output buffer, but allow it to store
	 * more bytes, as long as there's still source bytes and room in the
	 * output buffer.
	 */

	char *dstStart, *dstMax;
	const char *srcStart;

	dstStart = dst;
	dstMax = dst + *dstLenPtr;

	srcStart = src;

	if (srcLen < *dstLenPtr) {
	    dstEnd = dst + srcLen;
	} else {
	    dstEnd = dst + *dstLenPtr;
	}
	while (dst < dstEnd) {
	    if (*src == '\n') {
		if (dstEnd < dstMax) {
		    dstEnd++;
		}
		*dst++ = '\r';
		newlineFound = 1;
	    }
	    *dst++ = *src++;
	}
	*srcLenPtr = src - srcStart;
	*dstLenPtr = dst - dstStart;
	break;
    }
    default:
	break;
    }
    return newlineFound;
}

/*
 *---------------------------------------------------------------------------
 *
 * CheckFlush --
 *
 *	Helper function for WriteBytes() and WriteChars(). If the channel
 *	buffer is ready to be flushed, flush it.
 *
 * Results:
 *	The return value is -1 if there was a problem flushing the channel
 *	buffer, or 0 otherwise.
 *
 * Side effects:
 *	The buffer will be recycled if it is flushed.
 *
 *---------------------------------------------------------------------------
 */

static int
CheckFlush(
    Channel *chanPtr,		/* Channel being read, for buffering mode. */
    ChannelBuffer *bufPtr,	/* Channel buffer to possibly flush. */
    int newlineFlag)		/* Non-zero if a the channel buffer contains a
				 * newline. */
{
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */

    /*
     * The current buffer is ready for output:
     * 1. if it is full.
     * 2. if it contains a newline and this channel is line-buffered.
     * 3. if it contains any output and this channel is unbuffered.
     */

    if ((statePtr->flags & BUFFER_READY) == 0) {
	if (IsBufferFull(bufPtr)) {
	    SetFlag(statePtr, BUFFER_READY);
	} else if (statePtr->flags & CHANNEL_LINEBUFFERED) {
	    if (newlineFlag != 0) {
		SetFlag(statePtr, BUFFER_READY);
	    }
	} else if (statePtr->flags & CHANNEL_UNBUFFERED) {
	    SetFlag(statePtr, BUFFER_READY);
	}
    }
    if (statePtr->flags & BUFFER_READY) {
	if (FlushChannel(NULL, chanPtr, 0) != 0) {
	    return -1;
	}
    }
    return 0;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_Gets --
 *
 *	Reads a complete line of input from the channel into a Tcl_DString.
 *
 * Results:
 *	Length of line read (in characters) or -1 if error, EOF, or blocked.
 *	If -1, use Tcl_GetErrno() to retrieve the POSIX error code for the
 *	error or condition that occurred.
 *
 * Side effects:
 *	May flush output on the channel. May cause input to be consumed from
 *	the channel.
 *
 *---------------------------------------------------------------------------
 */

int
Tcl_Gets(
    Tcl_Channel chan,		/* Channel from which to read. */
    Tcl_DString *lineRead)	/* The line read will be appended to this
				 * DString as UTF-8 characters. The caller
				 * must have initialized it and is responsible
				 * for managing the storage. */
{
    Tcl_Obj *objPtr;
    int charsStored, length;
    char *string;

    TclNewObj(objPtr);
    charsStored = Tcl_GetsObj(chan, objPtr);
    if (charsStored > 0) {
	string = TclGetStringFromObj(objPtr, &length);
	Tcl_DStringAppend(lineRead, string, length);
    }
    TclDecrRefCount(objPtr);
    return charsStored;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_GetsObj --
 *
 *	Accumulate input from the input channel until end-of-line or
 *	end-of-file has been seen. Bytes read from the input channel are
 *	converted to UTF-8 using the encoding specified by the channel.
 *
 * Results:
 *	Number of characters accumulated in the object or -1 if error,
 *	blocked, or EOF. If -1, use Tcl_GetErrno() to retrieve the POSIX error
 *	code for the error or condition that occurred.
 *
 * Side effects:
 *	Consumes input from the channel.
 *
 *	On reading EOF, leave channel pointing at EOF char. On reading EOL,
 *	leave channel pointing after EOL, but don't return EOL in dst buffer.
 *
 *---------------------------------------------------------------------------
 */

int
Tcl_GetsObj(
    Tcl_Channel chan,		/* Channel from which to read. */
    Tcl_Obj *objPtr)		/* The line read will be appended to this
				 * object as UTF-8 characters. */
{
    GetsState gs;
    Channel *chanPtr = (Channel *) chan;
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    ChannelBuffer *bufPtr;
    int inEofChar, skip, copiedTotal, oldLength, oldFlags, oldRemoved;
    Tcl_Encoding encoding;
    char *dst, *dstEnd, *eol, *eof;
    Tcl_EncodingState oldState;

    if (CheckChannelErrors(statePtr, TCL_READABLE) != 0) {
	copiedTotal = -1;
	goto done;
    }

    /*
     * A binary version of Tcl_GetsObj. This could also handle encodings that
     * are ascii-7 pure (iso8859, utf-8, ...) with a final encoding conversion
     * done on objPtr.
     */

    if ((statePtr->encoding == NULL)
	    && ((statePtr->inputTranslation == TCL_TRANSLATE_LF)
		    || (statePtr->inputTranslation == TCL_TRANSLATE_CR))) {
	return TclGetsObjBinary(chan, objPtr);
    }

    /*
     * This operation should occur at the top of a channel stack.
     */

    chanPtr = statePtr->topChanPtr;

    bufPtr = statePtr->inQueueHead;
    encoding = statePtr->encoding;

    /*
     * Preserved so we can restore the channel's state in case we don't find a
     * newline in the available input.
     */

    TclGetStringFromObj(objPtr, &oldLength);
    oldFlags = statePtr->inputEncodingFlags;
    oldState = statePtr->inputEncodingState;
    oldRemoved = BUFFER_PADDING;
    if (bufPtr != NULL) {
	oldRemoved = bufPtr->nextRemoved;
    }

    /*
     * If there is no encoding, use "iso8859-1" -- Tcl_GetsObj() doesn't
     * produce ByteArray objects.
     */

    if (encoding == NULL) {
	ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

	if (tsdPtr->binaryEncoding == NULL) {
	    tsdPtr->binaryEncoding = Tcl_GetEncoding(NULL, "iso8859-1");
	    Tcl_CreateThreadExitHandler(FreeBinaryEncoding, NULL);
	}
	encoding = tsdPtr->binaryEncoding;
	if (encoding == NULL) {
	    Tcl_Panic("attempted gets on binary channel where no iso8859-1 encoding available");
	}
    }

    /*
     * Object used by FilterInputBytes to keep track of how much data has been
     * consumed from the channel buffers.
     */

    gs.objPtr		= objPtr;
    gs.dstPtr		= &dst;
    gs.encoding		= encoding;
    gs.bufPtr		= bufPtr;
    gs.state		= oldState;
    gs.rawRead		= 0;
    gs.bytesWrote	= 0;
    gs.charsWrote	= 0;
    gs.totalChars	= 0;

    dst = objPtr->bytes + oldLength;
    dstEnd = dst;

    skip = 0;
    eof = NULL;
    inEofChar = statePtr->inEofChar;

    while (1) {
	if (dst >= dstEnd) {
	    if (FilterInputBytes(chanPtr, &gs) != 0) {
		goto restore;
	    }
	    dstEnd = dst + gs.bytesWrote;
	}

	/*
	 * Remember if EOF char is seen, then look for EOL anyhow, because the
	 * EOL might be before the EOF char.
	 */

	if (inEofChar != '\0') {
	    for (eol = dst; eol < dstEnd; eol++) {
		if (*eol == inEofChar) {
		    dstEnd = eol;
		    eof = eol;
		    break;
		}
	    }
	}

	/*
	 * On EOL, leave current file position pointing after the EOL, but
	 * don't store the EOL in the output string.
	 */

	switch (statePtr->inputTranslation) {
	case TCL_TRANSLATE_LF:
	    for (eol = dst; eol < dstEnd; eol++) {
		if (*eol == '\n') {
		    skip = 1;
		    goto gotEOL;
		}
	    }
	    break;
	case TCL_TRANSLATE_CR:
	    for (eol = dst; eol < dstEnd; eol++) {
		if (*eol == '\r') {
		    skip = 1;
		    goto gotEOL;
		}
	    }
	    break;
	case TCL_TRANSLATE_CRLF:
	    for (eol = dst; eol < dstEnd; eol++) {
		if (*eol == '\r') {
		    eol++;

		    /*
		     * If a CR is at the end of the buffer, then check for a
		     * LF at the begining of the next buffer, unless EOF char
		     * was found already.
		     */

		    if (eol >= dstEnd) {
			int offset;

			if (eol != eof) {
			    offset = eol - objPtr->bytes;
			    dst = dstEnd;
			    if (FilterInputBytes(chanPtr, &gs) != 0) {
				goto restore;
			    }
			    dstEnd = dst + gs.bytesWrote;
			    eol = objPtr->bytes + offset;
			}
			if (eol >= dstEnd) {
			    skip = 0;
			    goto gotEOL;
			}
		    }
		    if (*eol == '\n') {
			eol--;
			skip = 2;
			goto gotEOL;
		    }
		}
	    }
	    break;
	case TCL_TRANSLATE_AUTO:
	    eol = dst;
	    skip = 1;
	    if (statePtr->flags & INPUT_SAW_CR) {
		ResetFlag(statePtr, INPUT_SAW_CR);
		if ((eol < dstEnd) && (*eol == '\n')) {
		    /*
		     * Skip the raw bytes that make up the '\n'.
		     */

		    char tmp[1 + TCL_UTF_MAX];
		    int rawRead;

		    bufPtr = gs.bufPtr;
		    Tcl_ExternalToUtf(NULL, gs.encoding, RemovePoint(bufPtr),
			    gs.rawRead, statePtr->inputEncodingFlags,
			    &gs.state, tmp, 1 + TCL_UTF_MAX, &rawRead, NULL,
			    NULL);
		    bufPtr->nextRemoved += rawRead;
		    gs.rawRead -= rawRead;
		    gs.bytesWrote--;
		    gs.charsWrote--;
		    memmove(dst, dst + 1, (size_t) (dstEnd - dst));
		    dstEnd--;
		}
	    }
	    for (eol = dst; eol < dstEnd; eol++) {
		if (*eol == '\r') {
		    eol++;
		    if (eol == dstEnd) {
			/*
			 * If buffer ended on \r, peek ahead to see if a \n is
			 * available, unless EOF char was found already.
			 */

			if (eol != eof) {
			    int offset;

			    offset = eol - objPtr->bytes;
			    dst = dstEnd;
			    PeekAhead(chanPtr, &dstEnd, &gs);
			    eol = objPtr->bytes + offset;
			}

			if (eol >= dstEnd) {
			    eol--;
			    SetFlag(statePtr, INPUT_SAW_CR);
			    goto gotEOL;
			}
		    }
		    if (*eol == '\n') {
			skip++;
		    }
		    eol--;
		    goto gotEOL;
		} else if (*eol == '\n') {
		    goto gotEOL;
		}
	    }
	}
	if (eof != NULL) {
	    /*
	     * EOF character was seen. On EOF, leave current file position
	     * pointing at the EOF character, but don't store the EOF
	     * character in the output string.
	     */

	    dstEnd = eof;
	    SetFlag(statePtr, CHANNEL_EOF | CHANNEL_STICKY_EOF);
	    statePtr->inputEncodingFlags |= TCL_ENCODING_END;
	}
	if (statePtr->flags & CHANNEL_EOF) {
	    skip = 0;
	    eol = dstEnd;
	    if (eol == objPtr->bytes + oldLength) {
		/*
		 * If we didn't append any bytes before encountering EOF,
		 * caller needs to see -1.
		 */

		Tcl_SetObjLength(objPtr, oldLength);
		CommonGetsCleanup(chanPtr);
		copiedTotal = -1;
		goto done;
	    }
	    goto gotEOL;
	}
	dst = dstEnd;
    }

    /*
     * Found EOL or EOF, but the output buffer may now contain too many UTF-8
     * characters. We need to know how many raw bytes correspond to the number
     * of UTF-8 characters we want, plus how many raw bytes correspond to the
     * character(s) making up EOL (if any), so we can remove the correct
     * number of bytes from the channel buffer.
     */

  gotEOL:
    bufPtr = gs.bufPtr;
    if (bufPtr == NULL) {
	Tcl_Panic("Tcl_GetsObj: gotEOL reached with bufPtr==NULL");
    }
    statePtr->inputEncodingState = gs.state;
    Tcl_ExternalToUtf(NULL, gs.encoding, RemovePoint(bufPtr), gs.rawRead,
	    statePtr->inputEncodingFlags, &statePtr->inputEncodingState, dst,
	    eol - dst + skip + TCL_UTF_MAX, &gs.rawRead, NULL,
	    &gs.charsWrote);
    bufPtr->nextRemoved += gs.rawRead;

    /*
     * Recycle all the emptied buffers.
     */

    Tcl_SetObjLength(objPtr, eol - objPtr->bytes);
    CommonGetsCleanup(chanPtr);
    ResetFlag(statePtr, CHANNEL_BLOCKED);
    copiedTotal = gs.totalChars + gs.charsWrote - skip;
    goto done;

    /*
     * Couldn't get a complete line. This only happens if we get a error
     * reading from the channel or we are non-blocking and there wasn't an EOL
     * or EOF in the data available.
     */

  restore:
    bufPtr = statePtr->inQueueHead;
    if (bufPtr == NULL) {
	Tcl_Panic("Tcl_GetsObj: restore reached with bufPtr==NULL");
    }
    bufPtr->nextRemoved = oldRemoved;

    for (bufPtr = bufPtr->nextPtr; bufPtr != NULL; bufPtr = bufPtr->nextPtr) {
	bufPtr->nextRemoved = BUFFER_PADDING;
    }
    CommonGetsCleanup(chanPtr);

    statePtr->inputEncodingState = oldState;
    statePtr->inputEncodingFlags = oldFlags;
    Tcl_SetObjLength(objPtr, oldLength);

    /*
     * We didn't get a complete line so we need to indicate to UpdateInterest
     * that the gets blocked. It will wait for more data instead of firing a
     * timer, avoiding a busy wait. This is where we are assuming that the
     * next operation is a gets. No more file events will be delivered on this
     * channel until new data arrives or some operation is performed on the
     * channel (e.g. gets, read, fconfigure) that changes the blocking state.
     * Note that this means a file event will not be delivered even though a
     * read would be able to consume the buffered data.
     */

    SetFlag(statePtr, CHANNEL_NEED_MORE_DATA);
    copiedTotal = -1;

    /*
     * Update the notifier state so we don't block while there is still data
     * in the buffers.
     */

  done:
    UpdateInterest(chanPtr);
    return copiedTotal;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclGetsObjBinary --
 *
 *	A variation of Tcl_GetsObj that works directly on the buffers until
 *	end-of-line or end-of-file has been seen. Bytes read from the input
 *	channel return as a ByteArray obj.
 *
 * Results:
 *	Number of characters accumulated in the object or -1 if error,
 *	blocked, or EOF. If -1, use Tcl_GetErrno() to retrieve the POSIX error
 *	code for the error or condition that occurred.
 *
 * Side effects:
 *	Consumes input from the channel.
 *
 *	On reading EOF, leave channel pointing at EOF char. On reading EOL,
 *	leave channel pointing after EOL, but don't return EOL in dst buffer.
 *
 *---------------------------------------------------------------------------
 */

static int
TclGetsObjBinary(
    Tcl_Channel chan,		/* Channel from which to read. */
    Tcl_Obj *objPtr)		/* The line read will be appended to this
				 * object as UTF-8 characters. */
{
    Channel *chanPtr = (Channel *) chan;
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    ChannelBuffer *bufPtr;
    int inEofChar, skip, copiedTotal, oldLength, oldFlags, oldRemoved;
    int rawLen, byteLen, eolChar;
    unsigned char *dst, *dstEnd, *eol, *eof, *byteArray;

    /*
     * This operation should occur at the top of a channel stack.
     */

    chanPtr = statePtr->topChanPtr;

    bufPtr = statePtr->inQueueHead;

    /*
     * Preserved so we can restore the channel's state in case we don't find a
     * newline in the available input.
     */

    byteArray = Tcl_GetByteArrayFromObj(objPtr, &byteLen);
    oldFlags = statePtr->inputEncodingFlags;
    oldRemoved = BUFFER_PADDING;
    oldLength = byteLen;
    if (bufPtr != NULL) {
	oldRemoved = bufPtr->nextRemoved;
    }

    rawLen = 0;
    skip = 0;
    eof = NULL;
    inEofChar = statePtr->inEofChar;
    /* Only handle TCL_TRANSLATE_LF and TCL_TRANSLATE_CR */
    eolChar = (statePtr->inputTranslation == TCL_TRANSLATE_LF) ? '\n' : '\r';

    while (1) {
	/*
	 * Subtract the number of bytes that were removed from channel
	 * buffer during last call.
	 */

	if (bufPtr != NULL) {
	    bufPtr->nextRemoved += rawLen;
	    if (!IsBufferReady(bufPtr)) {
		bufPtr = bufPtr->nextPtr;
	    }
	}

	if ((bufPtr == NULL) || (bufPtr->nextAdded == BUFFER_PADDING)) {
	    /*
	     * All channel buffers were exhausted and the caller still
	     * hasn't seen EOL. Need to read more bytes from the channel
	     * device. Side effect is to allocate another channel buffer.
	     */

	    if (statePtr->flags & CHANNEL_BLOCKED) {
		if (statePtr->flags & CHANNEL_NONBLOCKING) {
		    goto restore;
		}
		ResetFlag(statePtr, CHANNEL_BLOCKED);
	    }
	    if (GetInput(chanPtr) != 0) {
		goto restore;
	    }
	    bufPtr = statePtr->inQueueTail;
	}

	dst = (unsigned char *) RemovePoint(bufPtr);
	dstEnd = dst + BytesLeft(bufPtr);

	/*
	 * Remember if EOF char is seen, then look for EOL anyhow, because the
	 * EOL might be before the EOF char.
	 * XXX - in the binary case, consider coincident search for eol/eof.
	 */

	if (inEofChar != '\0') {
	    for (eol = dst; eol < dstEnd; eol++) {
		if (*eol == inEofChar) {
		    dstEnd = eol;
		    eof = eol;
		    break;
		}
	    }
	}

	/*
	 * On EOL, leave current file position pointing after the EOL, but
	 * don't store the EOL in the output string.
	 */

	for (eol = dst; eol < dstEnd; eol++) {
	    if (*eol == eolChar) {
		skip = 1;
		goto gotEOL;
	    }
	}
	if (eof != NULL) {
	    /*
	     * EOF character was seen. On EOF, leave current file position
	     * pointing at the EOF character, but don't store the EOF
	     * character in the output string.
	     */

	    SetFlag(statePtr, CHANNEL_EOF | CHANNEL_STICKY_EOF);
	    statePtr->inputEncodingFlags |= TCL_ENCODING_END;
	}
	if (statePtr->flags & CHANNEL_EOF) {
	    skip = 0;
	    eol = dstEnd;
	    if ((dst == dstEnd) && (byteLen == oldLength)) {
		/*
		 * If we didn't append any bytes before encountering EOF,
		 * caller needs to see -1.
		 */

		byteArray = Tcl_SetByteArrayLength(objPtr, oldLength);
		CommonGetsCleanup(chanPtr);
		copiedTotal = -1;
		goto done;
	    }
	    goto gotEOL;
	}

	/*
	 * Copy bytes from the channel buffer to the ByteArray.
	 * This may realloc space, so keep track of result.
	 */

	rawLen = dstEnd - dst;
	byteArray = Tcl_SetByteArrayLength(objPtr, byteLen + rawLen);
	memcpy(byteArray + byteLen, dst, (size_t) rawLen);
	byteLen += rawLen;
    }

    /*
     * Found EOL or EOF, but the output buffer may now contain too many bytes.
     * We need to know how many bytes correspond to the number we want, so we
     * can remove the correct number of bytes from the channel buffer.
     */

  gotEOL:
    if (bufPtr == NULL) {
	Tcl_Panic("TclGetsObjBinary: gotEOL reached with bufPtr==NULL");
    }

    rawLen = eol - dst;
    byteArray = Tcl_SetByteArrayLength(objPtr, byteLen + rawLen);
    memcpy(byteArray + byteLen, dst, (size_t) rawLen);
    byteLen += rawLen;
    bufPtr->nextRemoved += rawLen + skip;

    /*
     * Convert the buffer if there was an encoding.
     * XXX - unimplemented.
     */

    if (statePtr->encoding != NULL) {
    }

    /*
     * Recycle all the emptied buffers.
     */

    CommonGetsCleanup(chanPtr);
    ResetFlag(statePtr, CHANNEL_BLOCKED);
    copiedTotal = byteLen;
    goto done;

    /*
     * Couldn't get a complete line. This only happens if we get a error
     * reading from the channel or we are non-blocking and there wasn't an EOL
     * or EOF in the data available.
     */

  restore:
    bufPtr = statePtr->inQueueHead;
    if (bufPtr == NULL) {
	Tcl_Panic("TclGetsObjBinary: restore reached with bufPtr==NULL");
    }
    bufPtr->nextRemoved = oldRemoved;

    for (bufPtr = bufPtr->nextPtr; bufPtr != NULL; bufPtr = bufPtr->nextPtr) {
	bufPtr->nextRemoved = BUFFER_PADDING;
    }
    CommonGetsCleanup(chanPtr);

    statePtr->inputEncodingFlags = oldFlags;
    byteArray = Tcl_SetByteArrayLength(objPtr, oldLength);

    /*
     * We didn't get a complete line so we need to indicate to UpdateInterest
     * that the gets blocked. It will wait for more data instead of firing a
     * timer, avoiding a busy wait. This is where we are assuming that the
     * next operation is a gets. No more file events will be delivered on this
     * channel until new data arrives or some operation is performed on the
     * channel (e.g. gets, read, fconfigure) that changes the blocking state.
     * Note that this means a file event will not be delivered even though a
     * read would be able to consume the buffered data.
     */

    SetFlag(statePtr, CHANNEL_NEED_MORE_DATA);
    copiedTotal = -1;

    /*
     * Update the notifier state so we don't block while there is still data
     * in the buffers.
     */

  done:
    UpdateInterest(chanPtr);
    return copiedTotal;
}

/*
 *---------------------------------------------------------------------------
 *
 * FreeBinaryEncoding --
 *
 *	Frees any "iso8859-1" Tcl_Encoding created by [gets] on a binary
 *	channel in a thread as part of that thread's finalization.
 *
 * Results:
 *	None.
 *
 *---------------------------------------------------------------------------
 */

static void
FreeBinaryEncoding(
    ClientData dummy)	/* Not used */
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    if (tsdPtr->binaryEncoding != NULL) {
	Tcl_FreeEncoding(tsdPtr->binaryEncoding);
	tsdPtr->binaryEncoding = NULL;
    }
}

/*
 *---------------------------------------------------------------------------
 *
 * FilterInputBytes --
 *
 *	Helper function for Tcl_GetsObj. Produces UTF-8 characters from raw
 *	bytes read from the channel.
 *
 *	Consumes available bytes from channel buffers. When channel buffers
 *	are exhausted, reads more bytes from channel device into a new channel
 *	buffer. It is the caller's responsibility to free the channel buffers
 *	that have been exhausted.
 *
 * Results:
 *	The return value is -1 if there was an error reading from the channel,
 *	0 otherwise.
 *
 * Side effects:
 *	Status object keeps track of how much data from channel buffers has
 *	been consumed and where UTF-8 bytes should be stored.
 *
 *---------------------------------------------------------------------------
 */

static int
FilterInputBytes(
    Channel *chanPtr,		/* Channel to read. */
    GetsState *gsPtr)		/* Current state of gets operation. */
{
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    ChannelBuffer *bufPtr;
    char *raw, *rawStart, *dst;
    int offset, toRead, dstNeeded, spaceLeft, result, rawLen, length;
    Tcl_Obj *objPtr;
#define ENCODING_LINESIZE 20	/* Lower bound on how many bytes to convert at
				 * a time. Since we don't know a priori how
				 * many bytes of storage this many source
				 * bytes will use, we actually need at least
				 * ENCODING_LINESIZE * TCL_MAX_UTF bytes of
				 * room. */

    objPtr = gsPtr->objPtr;

    /*
     * Subtract the number of bytes that were removed from channel buffer
     * during last call.
     */

    bufPtr = gsPtr->bufPtr;
    if (bufPtr != NULL) {
	bufPtr->nextRemoved += gsPtr->rawRead;
	if (!IsBufferReady(bufPtr)) {
	    bufPtr = bufPtr->nextPtr;
	}
    }
    gsPtr->totalChars += gsPtr->charsWrote;

    if ((bufPtr == NULL) || (bufPtr->nextAdded == BUFFER_PADDING)) {
	/*
	 * All channel buffers were exhausted and the caller still hasn't seen
	 * EOL. Need to read more bytes from the channel device. Side effect
	 * is to allocate another channel buffer.
	 */

    read:
	if (statePtr->flags & CHANNEL_BLOCKED) {
	    if (statePtr->flags & CHANNEL_NONBLOCKING) {
		gsPtr->charsWrote = 0;
		gsPtr->rawRead = 0;
		return -1;
	    }
	    ResetFlag(statePtr, CHANNEL_BLOCKED);
	}
	if (GetInput(chanPtr) != 0) {
	    gsPtr->charsWrote = 0;
	    gsPtr->rawRead = 0;
	    return -1;
	}
	bufPtr = statePtr->inQueueTail;
	gsPtr->bufPtr = bufPtr;
    }

    /*
     * Convert some of the bytes from the channel buffer to UTF-8. Space in
     * objPtr's string rep is used to hold the UTF-8 characters. Grow the
     * string rep if we need more space.
     */

    rawStart = RemovePoint(bufPtr);
    raw = rawStart;
    rawLen = BytesLeft(bufPtr);

    dst = *gsPtr->dstPtr;
    offset = dst - objPtr->bytes;
    toRead = ENCODING_LINESIZE;
    if (toRead > rawLen) {
	toRead = rawLen;
    }
    dstNeeded = toRead * TCL_UTF_MAX + 1;
    spaceLeft = objPtr->length - offset - TCL_UTF_MAX - 1;
    if (dstNeeded > spaceLeft) {
	length = offset * 2;
	if (offset < dstNeeded) {
	    length = offset + dstNeeded;
	}
	length += TCL_UTF_MAX + 1;
	Tcl_SetObjLength(objPtr, length);
	spaceLeft = length - offset;
	dst = objPtr->bytes + offset;
	*gsPtr->dstPtr = dst;
    }
    gsPtr->state = statePtr->inputEncodingState;
    result = Tcl_ExternalToUtf(NULL, gsPtr->encoding, raw, rawLen,
	    statePtr->inputEncodingFlags, &statePtr->inputEncodingState,
	    dst, spaceLeft, &gsPtr->rawRead, &gsPtr->bytesWrote,
	    &gsPtr->charsWrote);

    /*
     * Make sure that if we go through 'gets', that we reset the
     * TCL_ENCODING_START flag still. [Bug #523988]
     */

    statePtr->inputEncodingFlags &= ~TCL_ENCODING_START;

    if (result == TCL_CONVERT_MULTIBYTE) {
	/*
	 * The last few bytes in this channel buffer were the start of a
	 * multibyte sequence. If this buffer was full, then move them to the
	 * next buffer so the bytes will be contiguous.
	 */

	ChannelBuffer *nextPtr;
	int extra;

	nextPtr = bufPtr->nextPtr;
	if (!IsBufferFull(bufPtr)) {
	    if (gsPtr->rawRead > 0) {
		/*
		 * Some raw bytes were converted to UTF-8. Fall through,
		 * returning those UTF-8 characters because a EOL might be
		 * present in them.
		 */
	    } else if (statePtr->flags & CHANNEL_EOF) {
		/*
		 * There was a partial character followed by EOF on the
		 * device. Fall through, returning that nothing was found.
		 */

		bufPtr->nextRemoved = bufPtr->nextAdded;
	    } else {
		/*
		 * There are no more cached raw bytes left. See if we can get
		 * some more.
		 */

		goto read;
	    }
	} else {
	    if (nextPtr == NULL) {
		nextPtr = AllocChannelBuffer(statePtr->bufSize);
		bufPtr->nextPtr = nextPtr;
		statePtr->inQueueTail = nextPtr;
	    }
	    extra = rawLen - gsPtr->rawRead;
	    memcpy(nextPtr->buf + BUFFER_PADDING - extra,
		    raw + gsPtr->rawRead, (size_t) extra);
	    nextPtr->nextRemoved -= extra;
	    bufPtr->nextAdded -= extra;
	}
    }

    gsPtr->bufPtr = bufPtr;
    return 0;
}

/*
 *---------------------------------------------------------------------------
 *
 * PeekAhead --
 *
 *	Helper function used by Tcl_GetsObj(). Called when we've seen a \r at
 *	the end of the UTF-8 string and want to look ahead one character to
 *	see if it is a \n.
 *
 * Results:
 *	*gsPtr->dstPtr is filled with a pointer to the start of the range of
 *	UTF-8 characters that were found by peeking and *dstEndPtr is filled
 *	with a pointer to the bytes just after the end of the range.
 *
 * Side effects:
 *	If no more raw bytes were available in one of the channel buffers,
 *	tries to perform a non-blocking read to get more bytes from the
 *	channel device.
 *
 *---------------------------------------------------------------------------
 */

static void
PeekAhead(
    Channel *chanPtr,		/* The channel to read. */
    char **dstEndPtr,		/* Filled with pointer to end of new range of
				 * UTF-8 characters. */
    GetsState *gsPtr)		/* Current state of gets operation. */
{
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    ChannelBuffer *bufPtr;
    Tcl_DriverBlockModeProc *blockModeProc;
    int bytesLeft;

    bufPtr = gsPtr->bufPtr;

    /*
     * If there's any more raw input that's still buffered, we'll peek into
     * that. Otherwise, only get more data from the channel driver if it looks
     * like there might actually be more data. The assumption is that if the
     * channel buffer is filled right up to the end, then there might be more
     * data to read.
     */

    blockModeProc = NULL;
    if (bufPtr->nextPtr == NULL) {
	bytesLeft = BytesLeft(bufPtr) - gsPtr->rawRead;
	if (bytesLeft == 0) {
	    if (!IsBufferFull(bufPtr)) {
		/*
		 * Don't peek ahead if last read was short read.
		 */

		goto cleanup;
	    }
	    if ((statePtr->flags & CHANNEL_NONBLOCKING) == 0) {
		blockModeProc = Tcl_ChannelBlockModeProc(chanPtr->typePtr);
		if (blockModeProc == NULL) {
		    /*
		     * Don't peek ahead if cannot set non-blocking mode.
		     */

		    goto cleanup;
		}
		StackSetBlockMode(chanPtr, TCL_MODE_NONBLOCKING);
	    }
	}
    }
    if (FilterInputBytes(chanPtr, gsPtr) == 0) {
	*dstEndPtr = *gsPtr->dstPtr + gsPtr->bytesWrote;
    }
    if (blockModeProc != NULL) {
	StackSetBlockMode(chanPtr, TCL_MODE_BLOCKING);
    }
    return;

  cleanup:
    bufPtr->nextRemoved += gsPtr->rawRead;
    gsPtr->rawRead = 0;
    gsPtr->totalChars += gsPtr->charsWrote;
    gsPtr->bytesWrote = 0;
    gsPtr->charsWrote = 0;
}

/*
 *---------------------------------------------------------------------------
 *
 * CommonGetsCleanup --
 *
 *	Helper function for Tcl_GetsObj() to restore the channel after a
 *	"gets" operation.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Encoding may be freed.
 *
 *---------------------------------------------------------------------------
 */

static void
CommonGetsCleanup(
    Channel *chanPtr)
{
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    ChannelBuffer *bufPtr, *nextPtr;

    bufPtr = statePtr->inQueueHead;
    for ( ; bufPtr != NULL; bufPtr = nextPtr) {
	nextPtr = bufPtr->nextPtr;
	if (IsBufferReady(bufPtr)) {
	    break;
	}
	RecycleBuffer(statePtr, bufPtr, 0);
    }
    statePtr->inQueueHead = bufPtr;
    if (bufPtr == NULL) {
	statePtr->inQueueTail = NULL;
    } else {
	/*
	 * If any multi-byte characters were split across channel buffer
	 * boundaries, the split-up bytes were moved to the next channel
	 * buffer by FilterInputBytes(). Move the bytes back to their original
	 * buffer because the caller could change the channel's encoding which
	 * could change the interpretation of whether those bytes really made
	 * up multi-byte characters after all.
	 */

	nextPtr = bufPtr->nextPtr;
	for ( ; nextPtr != NULL; nextPtr = bufPtr->nextPtr) {
	    int extra;

	    extra = SpaceLeft(bufPtr);
	    if (extra > 0) {
		memcpy(InsertPoint(bufPtr),
			nextPtr->buf + BUFFER_PADDING - extra,
			(size_t) extra);
		bufPtr->nextAdded += extra;
		nextPtr->nextRemoved = BUFFER_PADDING;
	    }
	    bufPtr = nextPtr;
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_Read --
 *
 *	Reads a given number of bytes from a channel. EOL and EOF translation
 *	is done on the bytes being read, so the number of bytes consumed from
 *	the channel may not be equal to the number of bytes stored in the
 *	destination buffer.
 *
 *	No encoding conversions are applied to the bytes being read.
 *
 * Results:
 *	The number of bytes read, or -1 on error. Use Tcl_GetErrno() to
 *	retrieve the error code for the error that occurred.
 *
 * Side effects:
 *	May cause input to be buffered.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_Read(
    Tcl_Channel chan,		/* The channel from which to read. */
    char *dst,			/* Where to store input read. */
    int bytesToRead)		/* Maximum number of bytes to read. */
{
    Channel *chanPtr = (Channel *) chan;
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */

    /*
     * This operation should occur at the top of a channel stack.
     */

    chanPtr = statePtr->topChanPtr;

    if (CheckChannelErrors(statePtr, TCL_READABLE) != 0) {
	return -1;
    }

    return DoRead(chanPtr, dst, bytesToRead);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ReadRaw --
 *
 *	Reads a given number of bytes from a channel. EOL and EOF translation
 *	is done on the bytes being read, so the number of bytes consumed from
 *	the channel may not be equal to the number of bytes stored in the
 *	destination buffer.
 *
 *	No encoding conversions are applied to the bytes being read.
 *
 * Results:
 *	The number of bytes read, or -1 on error. Use Tcl_GetErrno() to
 *	retrieve the error code for the error that occurred.
 *
 * Side effects:
 *	May cause input to be buffered.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_ReadRaw(
    Tcl_Channel chan,		/* The channel from which to read. */
    char *bufPtr,		/* Where to store input read. */
    int bytesToRead)		/* Maximum number of bytes to read. */
{
    Channel *chanPtr = (Channel *) chan;
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    int nread, result, copied, copiedNow;

    /*
     * The check below does too much because it will reject a call to this
     * function with a channel which is part of an 'fcopy'. But we have to
     * allow this here or else the chaining in the transformation drivers will
     * fail with 'file busy' error instead of retrieving and transforming the
     * data to copy.
     *
     * We let the check procedure now believe that there is no fcopy in
     * progress. A better solution than this might be an additional flag
     * argument to switch off specific checks.
     */

    if (CheckChannelErrors(statePtr, TCL_READABLE | CHANNEL_RAW_MODE) != 0) {
	return -1;
    }

    /*
     * Check for information in the push-back buffers. If there is some, use
     * it. Go to the driver only if there is none (anymore) and the caller
     * requests more bytes.
     */

    for (copied = 0; copied < bytesToRead; copied += copiedNow) {
	copiedNow = CopyBuffer(chanPtr, bufPtr + copied,
		bytesToRead - copied);
	if (copiedNow == 0) {
	    if (statePtr->flags & CHANNEL_EOF) {
		goto done;
	    }
	    if (statePtr->flags & CHANNEL_BLOCKED) {
		if (statePtr->flags & CHANNEL_NONBLOCKING) {
		    goto done;
		}
		ResetFlag(statePtr, CHANNEL_BLOCKED);
	    }

#ifdef TCL_IO_TRACK_OS_FOR_DRIVER_WITH_BAD_BLOCKING
	    /*
	     * [Bug 943274]. Better emulation of non-blocking channels for
	     * channels without BlockModeProc, by keeping track of true
	     * fileevents generated by the OS == Data waiting and reading if
	     * and only if we are sure to have data.
	     */

	    if ((statePtr->flags & CHANNEL_NONBLOCKING) &&
		    (Tcl_ChannelBlockModeProc(chanPtr->typePtr) == NULL) &&
		    !(statePtr->flags & CHANNEL_HAS_MORE_DATA)) {
		/*
		 * We bypass the driver; it would block as no data is
		 * available.
		 */

		nread = -1;
		result = EWOULDBLOCK;
	    } else {
#endif /* TCL_IO_TRACK_OS_FOR_DRIVER_WITH_BAD_BLOCKING */

		/*
		 * Now go to the driver to get as much as is possible to fill
		 * the remaining request. Do all the error handling by
		 * ourselves. The code was stolen from 'GetInput' and slightly
		 * adapted (different return value here).
		 *
		 * The case of 'bytesToRead == 0' at this point cannot happen.
		 */

		nread = (chanPtr->typePtr->inputProc)(chanPtr->instanceData,
			bufPtr + copied, bytesToRead - copied, &result);

#ifdef TCL_IO_TRACK_OS_FOR_DRIVER_WITH_BAD_BLOCKING
	    }
#endif /* TCL_IO_TRACK_OS_FOR_DRIVER_WITH_BAD_BLOCKING */

	    if (nread > 0) {
		/*
		 * If we get a short read, signal up that we may be BLOCKED.
		 * We should avoid calling the driver because on some
		 * platforms we will block in the low level reading code even
		 * though the channel is set into nonblocking mode.
		 */

		if (nread < (bytesToRead - copied)) {
		    SetFlag(statePtr, CHANNEL_BLOCKED);
		}

#ifdef TCL_IO_TRACK_OS_FOR_DRIVER_WITH_BAD_BLOCKING
		if (nread <= (bytesToRead - copied)) {
		    /*
		     * [Bug 943274] We have read the available data, clear
		     * flag.
		     */

		    ResetFlag(statePtr, CHANNEL_HAS_MORE_DATA);
		}
#endif /* TCL_IO_TRACK_OS_FOR_DRIVER_WITH_BAD_BLOCKING */

	    } else if (nread == 0) {
		SetFlag(statePtr, CHANNEL_EOF);
		statePtr->inputEncodingFlags |= TCL_ENCODING_END;

	    } else if (nread < 0) {
		if ((result == EWOULDBLOCK) || (result == EAGAIN)) {
		    if (copied > 0) {
			/*
			 * Information that was copied earlier has precedence
			 * over EAGAIN/WOULDBLOCK handling.
			 */

			return copied;
		    }

		    SetFlag(statePtr, CHANNEL_BLOCKED);
		    result = EAGAIN;
		}

		Tcl_SetErrno(result);
		return -1;
	    }

	    return copied + nread;
	}
    }

  done:
    return copied;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_ReadChars --
 *
 *	Reads from the channel until the requested number of characters have
 *	been seen, EOF is seen, or the channel would block. EOL and EOF
 *	translation is done. If reading binary data, the raw bytes are wrapped
 *	in a Tcl byte array object. Otherwise, the raw bytes are converted to
 *	UTF-8 using the channel's current encoding and stored in a Tcl string
 *	object.
 *
 * Results:
 *	The number of characters read, or -1 on error. Use Tcl_GetErrno() to
 *	retrieve the error code for the error that occurred.
 *
 * Side effects:
 *	May cause input to be buffered.
 *
 *---------------------------------------------------------------------------
 */

int
Tcl_ReadChars(
    Tcl_Channel chan,		/* The channel to read. */
    Tcl_Obj *objPtr,		/* Input data is stored in this object. */
    int toRead,			/* Maximum number of characters to store, or
				 * -1 to read all available data (up to EOF or
				 * when channel blocks). */
    int appendFlag)		/* If non-zero, data read from the channel
				 * will be appended to the object. Otherwise,
				 * the data will replace the existing contents
				 * of the object. */
{
    Channel *chanPtr = (Channel *) chan;
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */

    /*
     * This operation should occur at the top of a channel stack.
     */

    chanPtr = statePtr->topChanPtr;

    if (CheckChannelErrors(statePtr, TCL_READABLE) != 0) {
	/*
	 * Update the notifier state so we don't block while there is still
	 * data in the buffers.
	 */

	UpdateInterest(chanPtr);
	return -1;
    }

    return DoReadChars(chanPtr, objPtr, toRead, appendFlag);
}
/*
 *---------------------------------------------------------------------------
 *
 * DoReadChars --
 *
 *	Reads from the channel until the requested number of characters have
 *	been seen, EOF is seen, or the channel would block. EOL and EOF
 *	translation is done. If reading binary data, the raw bytes are wrapped
 *	in a Tcl byte array object. Otherwise, the raw bytes are converted to
 *	UTF-8 using the channel's current encoding and stored in a Tcl string
 *	object.
 *
 * Results:
 *	The number of characters read, or -1 on error. Use Tcl_GetErrno() to
 *	retrieve the error code for the error that occurred.
 *
 * Side effects:
 *	May cause input to be buffered.
 *
 *---------------------------------------------------------------------------
 */

static int
DoReadChars(
    Channel *chanPtr,		/* The channel to read. */
    Tcl_Obj *objPtr,		/* Input data is stored in this object. */
    int toRead,			/* Maximum number of characters to store, or
				 * -1 to read all available data (up to EOF or
				 * when channel blocks). */
    int appendFlag)		/* If non-zero, data read from the channel
				 * will be appended to the object. Otherwise,
				 * the data will replace the existing contents
				 * of the object. */
{
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    ChannelBuffer *bufPtr;
    int offset, factor, copied, copiedNow, result;
    Tcl_Encoding encoding;
#define UTF_EXPANSION_FACTOR	1024

    /*
     * This operation should occur at the top of a channel stack.
     */

    chanPtr = statePtr->topChanPtr;
    encoding = statePtr->encoding;
    factor = UTF_EXPANSION_FACTOR;

    if (appendFlag == 0) {
	if (encoding == NULL) {
	    Tcl_SetByteArrayLength(objPtr, 0);
	} else {
	    Tcl_SetObjLength(objPtr, 0);

	    /*
	     * We're going to access objPtr->bytes directly, so we must ensure
	     * that this is actually a string object (otherwise it might have
	     * been pure Unicode).
	     */

	    TclGetString(objPtr);
	}
	offset = 0;
    } else {
	if (encoding == NULL) {
	    Tcl_GetByteArrayFromObj(objPtr, &offset);
	} else {
	    TclGetStringFromObj(objPtr, &offset);
	}
    }

    for (copied = 0; (unsigned) toRead > 0; ) {
	copiedNow = -1;
	if (statePtr->inQueueHead != NULL) {
	    if (encoding == NULL) {
		copiedNow = ReadBytes(statePtr, objPtr, toRead, &offset);
	    } else {
		copiedNow = ReadChars(statePtr, objPtr, toRead, &offset,
			&factor);
	    }

	    /*
	     * If the current buffer is empty recycle it.
	     */

	    bufPtr = statePtr->inQueueHead;
	    if (IsBufferEmpty(bufPtr)) {
		ChannelBuffer *nextPtr;

		nextPtr = bufPtr->nextPtr;
		RecycleBuffer(statePtr, bufPtr, 0);
		statePtr->inQueueHead = nextPtr;
		if (nextPtr == NULL) {
		    statePtr->inQueueTail = NULL;
		}
	    }
	}

	if (copiedNow < 0) {
	    if (statePtr->flags & CHANNEL_EOF) {
		break;
	    }
	    if (statePtr->flags & CHANNEL_BLOCKED) {
		if (statePtr->flags & CHANNEL_NONBLOCKING) {
		    break;
		}
		ResetFlag(statePtr, CHANNEL_BLOCKED);
	    }
	    result = GetInput(chanPtr);
	    if (result != 0) {
		if (result == EAGAIN) {
		    break;
		}
		copied = -1;
		goto done;
	    }
	} else {
	    copied += copiedNow;
	    toRead -= copiedNow;
	}
    }

    ResetFlag(statePtr, CHANNEL_BLOCKED);
    if (encoding == NULL) {
	Tcl_SetByteArrayLength(objPtr, offset);
    } else {
	Tcl_SetObjLength(objPtr, offset);
    }

    /*
     * Update the notifier state so we don't block while there is still data
     * in the buffers.
     */

  done:
    UpdateInterest(chanPtr);
    return copied;
}

/*
 *---------------------------------------------------------------------------
 *
 * ReadBytes --
 *
 *	Reads from the channel until the requested number of bytes have been
 *	seen, EOF is seen, or the channel would block. Bytes from the channel
 *	are stored in objPtr as a ByteArray object. EOL and EOF translation
 *	are done.
 *
 *	'bytesToRead' can safely be a very large number because space is only
 *	allocated to hold data read from the channel as needed.
 *
 * Results:
 *	The return value is the number of bytes appended to the object and
 *	*offsetPtr is filled with the total number of bytes in the object
 *	(greater than the return value if there were already bytes in the
 *	object).
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */

static int
ReadBytes(
    ChannelState *statePtr,	/* State of the channel to read. */
    Tcl_Obj *objPtr,		/* Input data is appended to this ByteArray
				 * object. Its length is how much space has
				 * been allocated to hold data, not how many
				 * bytes of data have been stored in the
				 * object. */
    int bytesToRead,		/* Maximum number of bytes to store, or < 0 to
				 * get all available bytes. Bytes are obtained
				 * from the first buffer in the queue - even
				 * if this number is larger than the number of
				 * bytes available in the first buffer, only
				 * the bytes from the first buffer are
				 * returned. */
    int *offsetPtr)		/* On input, contains how many bytes of objPtr
				 * have been used to hold data. On output,
				 * filled with how many bytes are now being
				 * used. */
{
    int toRead, srcLen, offset, length, srcRead, dstWrote;
    ChannelBuffer *bufPtr;
    char *src, *dst;

    offset = *offsetPtr;

    bufPtr = statePtr->inQueueHead;
    src = RemovePoint(bufPtr);
    srcLen = BytesLeft(bufPtr);

    toRead = bytesToRead;
    if ((unsigned) toRead > (unsigned) srcLen) {
	toRead = srcLen;
    }

    dst = (char *) Tcl_GetByteArrayFromObj(objPtr, &length);
    if (toRead > length - offset - 1) {
	/*
	 * Double the existing size of the object or make enough room to hold
	 * all the characters we may get from the source buffer, whichever is
	 * larger.
	 */

	length = offset * 2;
	if (offset < toRead) {
	    length = offset + toRead + 1;
	}
	dst = (char *) Tcl_SetByteArrayLength(objPtr, length);
    }
    dst += offset;

    if (statePtr->flags & INPUT_NEED_NL) {
	ResetFlag(statePtr, INPUT_NEED_NL);
	if ((srcLen == 0) || (*src != '\n')) {
	    *dst = '\r';
	    *offsetPtr += 1;
	    return 1;
	}
	*dst++ = '\n';
	src++;
	srcLen--;
	toRead--;
    }

    srcRead = srcLen;
    dstWrote = toRead;
    if (TranslateInputEOL(statePtr, dst, src, &dstWrote, &srcRead) != 0) {
	if (dstWrote == 0) {
	    return -1;
	}
    }
    bufPtr->nextRemoved += srcRead;
    *offsetPtr += dstWrote;
    return dstWrote;
}

/*
 *---------------------------------------------------------------------------
 *
 * ReadChars --
 *
 *	Reads from the channel until the requested number of UTF-8 characters
 *	have been seen, EOF is seen, or the channel would block. Raw bytes
 *	from the channel are converted to UTF-8 and stored in objPtr. EOL and
 *	EOF translation is done.
 *
 *	'charsToRead' can safely be a very large number because space is only
 *	allocated to hold data read from the channel as needed.
 *
 * Results:
 *	The return value is the number of characters appended to the object,
 *	*offsetPtr is filled with the number of bytes that were appended, and
 *	*factorPtr is filled with the expansion factor used to guess how many
 *	bytes of UTF-8 to allocate to hold N source bytes.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */

static int
ReadChars(
    ChannelState *statePtr,	/* State of channel to read. */
    Tcl_Obj *objPtr,		/* Input data is appended to this object.
				 * objPtr->length is how much space has been
				 * allocated to hold data, not how many bytes
				 * of data have been stored in the object. */
    int charsToRead,		/* Maximum number of characters to store, or
				 * -1 to get all available characters.
				 * Characters are obtained from the first
				 * buffer in the queue -- even if this number
				 * is larger than the number of characters
				 * available in the first buffer, only the
				 * characters from the first buffer are
				 * returned. */
    int *offsetPtr,		/* On input, contains how many bytes of objPtr
				 * have been used to hold data. On output,
				 * filled with how many bytes are now being
				 * used. */
    int *factorPtr)		/* On input, contains a guess of how many
				 * bytes need to be allocated to hold the
				 * result of converting N source bytes to
				 * UTF-8. On output, contains another guess
				 * based on the data seen so far. */
{
    int toRead, factor, offset, spaceLeft, length, srcLen, dstNeeded;
    int srcRead, dstWrote, numChars, dstRead;
    ChannelBuffer *bufPtr;
    char *src, *dst;
    Tcl_EncodingState oldState;
    int encEndFlagSuppressed = 0;

    factor = *factorPtr;
    offset = *offsetPtr;

    bufPtr = statePtr->inQueueHead;
    src = RemovePoint(bufPtr);
    srcLen = BytesLeft(bufPtr);

    toRead = charsToRead;
    if ((unsigned)toRead > (unsigned)srcLen) {
	toRead = srcLen;
    }

    /*
     * 'factor' is how much we guess that the bytes in the source buffer will
     * expand when converted to UTF-8 chars. This guess comes from analyzing
     * how many characters were produced by the previous pass.
     */

    dstNeeded = toRead * factor / UTF_EXPANSION_FACTOR;
    spaceLeft = objPtr->length - offset - TCL_UTF_MAX - 1;

    if (dstNeeded > spaceLeft) {
	/*
	 * Double the existing size of the object or make enough room to hold
	 * all the characters we want from the source buffer, whichever is
	 * larger.
	 */

	length = offset * 2;
	if (offset < dstNeeded) {
	    length = offset + dstNeeded;
	}
	spaceLeft = length - offset;
	length += TCL_UTF_MAX + 1;
	Tcl_SetObjLength(objPtr, length);
    }
    if (toRead == srcLen) {
	/*
	 * Want to convert the whole buffer in one pass. If we have enough
	 * space, convert it using all available space in object rather than
	 * using the factor.
	 */

	dstNeeded = spaceLeft;
    }
    dst = objPtr->bytes + offset;

    /*
     * [Bug 1462248]: The cause of the crash reported in this bug is this:
     *
     * - ReadChars, called with a single buffer, with a incomplete
     *	 multi-byte character at the end (only the first byte of it).
     * - Encoding translation fails, asks for more data
     * - Data is read, and eof is reached, TCL_ENCODING_END (TEE) is set.
     * - ReadChar is called again, converts the first buffer, but due to TEE
     *	 it does not check for incomplete multi-byte data, and the character
     *	 just after the end of the first buffer is a valid completion of the
     *	 multi-byte header in the actual buffer. The conversion reads more
     *	 characters from the buffer then present. This causes nextRemoved to
     *	 overshoot nextAdded and the next reads compute a negative srcLen,
     *	 cause further translations to fail, causing copying of data into the
     *	 next buffer using bad arguments, causing the mecpy for to eventually
     *	 fail.
     *
     * In the end it is a memory access bug spiraling out of control if the
     * conditions are _just so_. And ultimate cause is that TEE is given to a
     * conversion where it should not. TEE signals that this is the last
     * buffer. Except in our case it is not.
     *
     * My solution is to suppress TEE if the first buffer is not the last. We
     * will eventually need it given that EOF has been reached, but not right
     * now. This is what the new flag "endEncSuppressFlag" is for.
     *
     * The bug in 'Tcl_Utf2UtfProc' where it read from memory behind the
     * actual buffer has been fixed as well, and fixes the problem with the
     * crash too, but this would still allow the generic layer to
     * accidentially break a multi-byte sequence if the conditions are just
     * right, because again the ExternalToUtf would be successful where it
     * should not.
     */

    if ((statePtr->inputEncodingFlags & TCL_ENCODING_END) &&
	    (bufPtr->nextPtr != NULL)) {
	/*
	 * TEE is set for a buffer which is not the last. Squash it for now,
	 * and restore it later, before yielding control to our caller.
	 */

	statePtr->inputEncodingFlags &= ~TCL_ENCODING_END;
	encEndFlagSuppressed = 1;
    }

    oldState = statePtr->inputEncodingState;
    if (statePtr->flags & INPUT_NEED_NL) {
	/*
	 * We want a '\n' because the last character we saw was '\r'.
	 */

	ResetFlag(statePtr, INPUT_NEED_NL);
	Tcl_ExternalToUtf(NULL, statePtr->encoding, src, srcLen,
		statePtr->inputEncodingFlags, &statePtr->inputEncodingState,
		dst, TCL_UTF_MAX + 1, &srcRead, &dstWrote, &numChars);
	if ((dstWrote > 0) && (*dst == '\n')) {
	    /*
	     * The next char was a '\n'. Consume it and produce a '\n'.
	     */

	    bufPtr->nextRemoved += srcRead;
	} else {
	    /*
	     * The next char was not a '\n'. Produce a '\r'.
	     */

	    *dst = '\r';
	}
	statePtr->inputEncodingFlags &= ~TCL_ENCODING_START;
	*offsetPtr += 1;

	if (encEndFlagSuppressed) {
	    statePtr->inputEncodingFlags |= TCL_ENCODING_END;
	}
	return 1;
    }

    Tcl_ExternalToUtf(NULL, statePtr->encoding, src, srcLen,
	    statePtr->inputEncodingFlags, &statePtr->inputEncodingState, dst,
	    dstNeeded + TCL_UTF_MAX, &srcRead, &dstWrote, &numChars);

    if (encEndFlagSuppressed) {
	statePtr->inputEncodingFlags |= TCL_ENCODING_END;
    }

    if (srcRead == 0) {
	/*
	 * Not enough bytes in src buffer to make a complete char. Copy the
	 * bytes to the next buffer to make a new contiguous string, then tell
	 * the caller to fill the buffer with more bytes.
	 */

	ChannelBuffer *nextPtr;

	nextPtr = bufPtr->nextPtr;
	if (nextPtr == NULL) {
	    if (srcLen > 0) {
		/*
		 * There isn't enough data in the buffers to complete the next
		 * character, so we need to wait for more data before the next
		 * file event can be delivered. [Bug 478856]
		 *
		 * The exception to this is if the input buffer was completely
		 * empty before we tried to convert its contents. Nothing in,
		 * nothing out, and no incomplete character data. The
		 * conversion before the current one was complete.
		 */

		SetFlag(statePtr, CHANNEL_NEED_MORE_DATA);
	    }
	    return -1;
	}

	/*
	 * Space is made at the beginning of the buffer to copy the previous
	 * unused bytes there. Check first if the buffer we are using actually
	 * has enough space at its beginning for the data we are copying.
	 * Because if not we will write over the buffer management
	 * information, especially the 'nextPtr'.
	 *
	 * Note that the BUFFER_PADDING (See AllocChannelBuffer) is used to
	 * prevent exactly this situation. I.e. it should never happen.
	 * Therefore it is ok to panic should it happen despite the
	 * precautions.
	 */

	if (nextPtr->nextRemoved - srcLen < 0) {
	    Tcl_Panic("Buffer Underflow, BUFFER_PADDING not enough");
	}

	nextPtr->nextRemoved -= srcLen;
	memcpy(RemovePoint(nextPtr), src, (size_t) srcLen);
	RecycleBuffer(statePtr, bufPtr, 0);
	statePtr->inQueueHead = nextPtr;
	return ReadChars(statePtr, objPtr, charsToRead, offsetPtr, factorPtr);
    }

    dstRead = dstWrote;
    if (TranslateInputEOL(statePtr, dst, dst, &dstWrote, &dstRead) != 0) {
	/*
	 * Hit EOF char. How many bytes of src correspond to where the EOF was
	 * located in dst? Run the conversion again with an output buffer just
	 * big enough to hold the data so we can get the correct value for
	 * srcRead.
	 */

	if (dstWrote == 0) {
	    return -1;
	}
	statePtr->inputEncodingState = oldState;
	Tcl_ExternalToUtf(NULL, statePtr->encoding, src, srcLen,
		statePtr->inputEncodingFlags, &statePtr->inputEncodingState,
		dst, dstRead + TCL_UTF_MAX, &srcRead, &dstWrote, &numChars);
	TranslateInputEOL(statePtr, dst, dst, &dstWrote, &dstRead);
    }

    /*
     * The number of characters that we got may be less than the number that
     * we started with because "\r\n" sequences may have been turned into just
     * '\n' in dst.
     */

    numChars -= (dstRead - dstWrote);

    if ((unsigned) numChars > (unsigned) toRead) {
	/*
	 * Got too many chars.
	 */

	const char *eof;

	eof = Tcl_UtfAtIndex(dst, toRead);
	statePtr->inputEncodingState = oldState;
	Tcl_ExternalToUtf(NULL, statePtr->encoding, src, srcLen,
		statePtr->inputEncodingFlags, &statePtr->inputEncodingState,
		dst, eof - dst + TCL_UTF_MAX, &srcRead, &dstWrote, &numChars);
	dstRead = dstWrote;
	TranslateInputEOL(statePtr, dst, dst, &dstWrote, &dstRead);
	numChars -= (dstRead - dstWrote);
    }
    statePtr->inputEncodingFlags &= ~TCL_ENCODING_START;

    bufPtr->nextRemoved += srcRead;
    if (dstWrote > srcRead + 1) {
	*factorPtr = dstWrote * UTF_EXPANSION_FACTOR / srcRead;
    }
    *offsetPtr += dstWrote;
    return numChars;
}

/*
 *---------------------------------------------------------------------------
 *
 * TranslateInputEOL --
 *
 *	Perform input EOL and EOF translation on the source buffer, leaving
 *	the translated result in the destination buffer.
 *
 * Results:
 *	The return value is 1 if the EOF character was found when copying
 *	bytes to the destination buffer, 0 otherwise.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */

static int
TranslateInputEOL(
    ChannelState *statePtr,	/* Channel being read, for EOL translation and
				 * EOF character. */
    char *dstStart,		/* Output buffer filled with chars by applying
				 * appropriate EOL translation to source
				 * characters. */
    const char *srcStart,	/* Source characters. */
    int *dstLenPtr,		/* On entry, the maximum length of output
				 * buffer in bytes; must be <= *srcLenPtr. On
				 * exit, the number of bytes actually used in
				 * output buffer. */
    int *srcLenPtr)		/* On entry, the length of source buffer. On
				 * exit, the number of bytes read from the
				 * source buffer. */
{
    int dstLen, srcLen, inEofChar;
    const char *eof;

    dstLen = *dstLenPtr;

    eof = NULL;
    inEofChar = statePtr->inEofChar;
    if (inEofChar != '\0') {
	/*
	 * Find EOF in translated buffer then compress out the EOL. The source
	 * buffer may be much longer than the destination buffer - we only
	 * want to return EOF if the EOF has been copied to the destination
	 * buffer.
	 */

	const char *src, *srcMax;

	srcMax = srcStart + *srcLenPtr;
	for (src = srcStart; src < srcMax; src++) {
	    if (*src == inEofChar) {
		eof = src;
		srcLen = src - srcStart;
		if (srcLen < dstLen) {
		    dstLen = srcLen;
		}
		*srcLenPtr = srcLen;
		break;
	    }
	}
    }
    switch (statePtr->inputTranslation) {
    case TCL_TRANSLATE_LF:
	if (dstStart != srcStart) {
	    memcpy(dstStart, srcStart, (size_t) dstLen);
	}
	srcLen = dstLen;
	break;
    case TCL_TRANSLATE_CR: {
	char *dst, *dstEnd;

	if (dstStart != srcStart) {
	    memcpy(dstStart, srcStart, (size_t) dstLen);
	}
	dstEnd = dstStart + dstLen;
	for (dst = dstStart; dst < dstEnd; dst++) {
	    if (*dst == '\r') {
		*dst = '\n';
	    }
	}
	srcLen = dstLen;
	break;
    }
    case TCL_TRANSLATE_CRLF: {
	char *dst;
	const char *src, *srcEnd, *srcMax;

	dst = dstStart;
	src = srcStart;
	srcEnd = srcStart + dstLen;
	srcMax = srcStart + *srcLenPtr;

	for ( ; src < srcEnd; ) {
	    if (*src == '\r') {
		src++;
		if (src >= srcMax) {
		    SetFlag(statePtr, INPUT_NEED_NL);
		} else if (*src == '\n') {
		    *dst++ = *src++;
		} else {
		    *dst++ = '\r';
		}
	    } else {
		*dst++ = *src++;
	    }
	}
	srcLen = src - srcStart;
	dstLen = dst - dstStart;
	break;
    }
    case TCL_TRANSLATE_AUTO: {
	char *dst;
	const char *src, *srcEnd, *srcMax;

	dst = dstStart;
	src = srcStart;
	srcEnd = srcStart + dstLen;
	srcMax = srcStart + *srcLenPtr;

	if ((statePtr->flags & INPUT_SAW_CR) && (src < srcMax)) {
	    if (*src == '\n') {
		src++;
	    }
	    ResetFlag(statePtr, INPUT_SAW_CR);
	}
	for ( ; src < srcEnd; ) {
	    if (*src == '\r') {
		src++;
		if (src >= srcMax) {
		    SetFlag(statePtr, INPUT_SAW_CR);
		} else if (*src == '\n') {
		    if (srcEnd < srcMax) {
			srcEnd++;
		    }
		    src++;
		}
		*dst++ = '\n';
	    } else {
		*dst++ = *src++;
	    }
	}
	srcLen = src - srcStart;
	dstLen = dst - dstStart;
	break;
    }
    default:
	return 0;
    }
    *dstLenPtr = dstLen;

    if ((eof != NULL) && (srcStart + srcLen >= eof)) {
	/*
	 * EOF character was seen in EOL translated range. Leave current file
	 * position pointing at the EOF character, but don't store the EOF
	 * character in the output string.
	 */

	SetFlag(statePtr, CHANNEL_EOF | CHANNEL_STICKY_EOF);
	statePtr->inputEncodingFlags |= TCL_ENCODING_END;
	ResetFlag(statePtr, INPUT_SAW_CR | INPUT_NEED_NL);
	return 1;
    }

    *srcLenPtr = srcLen;
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_Ungets --
 *
 *	Causes the supplied string to be added to the input queue of the
 *	channel, at either the head or tail of the queue.
 *
 * Results:
 *	The number of bytes stored in the channel, or -1 on error.
 *
 * Side effects:
 *	Adds input to the input queue of a channel.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_Ungets(
    Tcl_Channel chan,		/* The channel for which to add the input. */
    const char *str,		/* The input itself. */
    int len,			/* The length of the input. */
    int atEnd)			/* If non-zero, add at end of queue; otherwise
				 * add at head of queue. */
{
    Channel *chanPtr;		/* The real IO channel. */
    ChannelState *statePtr;	/* State of actual channel. */
    ChannelBuffer *bufPtr;	/* Buffer to contain the data. */
    int flags;

    chanPtr = (Channel *) chan;
    statePtr = chanPtr->state;

    /*
     * This operation should occur at the top of a channel stack.
     */

    chanPtr = statePtr->topChanPtr;

    /*
     * CheckChannelErrors clears too many flag bits in this one case.
     */

    flags = statePtr->flags;
    if (CheckChannelErrors(statePtr, TCL_READABLE) != 0) {
	len = -1;
	goto done;
    }
    statePtr->flags = flags;

    /*
     * If we have encountered a sticky EOF, just punt without storing (sticky
     * EOF is set if we have seen the input eofChar, to prevent reading beyond
     * the eofChar). Otherwise, clear the EOF flags, and clear the BLOCKED
     * bit. We want to discover these conditions anew in each operation.
     */

    if (statePtr->flags & CHANNEL_STICKY_EOF) {
	goto done;
    }
    ResetFlag(statePtr, CHANNEL_BLOCKED | CHANNEL_EOF);

    bufPtr = AllocChannelBuffer(len);
    memcpy(InsertPoint(bufPtr), str, (size_t) len);
    bufPtr->nextAdded += len;

    if (statePtr->inQueueHead == NULL) {
	bufPtr->nextPtr = NULL;
	statePtr->inQueueHead = bufPtr;
	statePtr->inQueueTail = bufPtr;
    } else if (atEnd) {
	bufPtr->nextPtr = NULL;
	statePtr->inQueueTail->nextPtr = bufPtr;
	statePtr->inQueueTail = bufPtr;
    } else {
	bufPtr->nextPtr = statePtr->inQueueHead;
	statePtr->inQueueHead = bufPtr;
    }

    /*
     * Update the notifier state so we don't block while there is still data
     * in the buffers.
     */

  done:
    UpdateInterest(chanPtr);
    return len;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_Flush --
 *
 *	Flushes output data on a channel.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	May flush output queued on this channel.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_Flush(
    Tcl_Channel chan)		/* The Channel to flush. */
{
    int result;			/* Of calling FlushChannel. */
    Channel *chanPtr = (Channel *) chan;
				/* The actual channel. */
    ChannelState *statePtr = chanPtr->state;
				/* State of actual channel. */

    /*
     * This operation should occur at the top of a channel stack.
     */

    chanPtr = statePtr->topChanPtr;

    if (CheckChannelErrors(statePtr, TCL_WRITABLE) != 0) {
	return -1;
    }

    /*
     * Force current output buffer to be output also.
     */

    if ((statePtr->curOutPtr != NULL) && IsBufferReady(statePtr->curOutPtr)) {
	SetFlag(statePtr, BUFFER_READY);
    }

    result = FlushChannel(NULL, chanPtr, 0);
    if (result != 0) {
	return TCL_ERROR;
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * DiscardInputQueued --
 *
 *	Discards any input read from the channel but not yet consumed by Tcl
 *	reading commands.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	May discard input from the channel. If discardLastBuffer is zero,
 *	leaves one buffer in place for back-filling.
 *
 *----------------------------------------------------------------------
 */

static void
DiscardInputQueued(
    ChannelState *statePtr,	/* Channel on which to discard the queued
				 * input. */
    int discardSavedBuffers)	/* If non-zero, discard all buffers including
				 * last one. */
{
    ChannelBuffer *bufPtr, *nxtPtr;
				/* Loop variables. */

    bufPtr = statePtr->inQueueHead;
    statePtr->inQueueHead = NULL;
    statePtr->inQueueTail = NULL;
    for (; bufPtr != NULL; bufPtr = nxtPtr) {
	nxtPtr = bufPtr->nextPtr;
	RecycleBuffer(statePtr, bufPtr, discardSavedBuffers);
    }

    /*
     * If discardSavedBuffers is nonzero, must also discard any previously
     * saved buffer in the saveInBufPtr field.
     */

    if (discardSavedBuffers && statePtr->saveInBufPtr != NULL) {
	ckfree((char *) statePtr->saveInBufPtr);
	statePtr->saveInBufPtr = NULL;
    }
}

/*
 *---------------------------------------------------------------------------
 *
 * GetInput --
 *
 *	Reads input data from a device into a channel buffer.
 *
 * Results:
 *	The return value is the Posix error code if an error occurred while
 *	reading from the file, or 0 otherwise.
 *
 * Side effects:
 *	Reads from the underlying device.
 *
 *---------------------------------------------------------------------------
 */

static int
GetInput(
    Channel *chanPtr)		/* Channel to read input from. */
{
    int toRead;			/* How much to read? */
    int result;			/* Of calling driver. */
    int nread;			/* How much was read from channel? */
    ChannelBuffer *bufPtr;	/* New buffer to add to input queue. */
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */

    /*
     * Prevent reading from a dead channel -- a channel that has been closed
     * but not yet deallocated, which can happen if the exit handler for
     * channel cleanup has run but the channel is still registered in some
     * interpreter.
     */

    if (CheckForDeadChannel(NULL, statePtr)) {
	return EINVAL;
    }

    /*
     * First check for more buffers in the pushback area of the topmost
     * channel in the stack and use them. They can be the result of a
     * transformation which went away without reading all the information
     * placed in the area when it was stacked.
     *
     * Two possibilities for the state: No buffers in it, or a single empty
     * buffer. In the latter case we can recycle it now.
     */

    if (chanPtr->inQueueHead != NULL) {
	if (statePtr->inQueueHead != NULL) {
	    RecycleBuffer(statePtr, statePtr->inQueueHead, 0);
	    statePtr->inQueueHead = NULL;
	}

	statePtr->inQueueHead = chanPtr->inQueueHead;
	statePtr->inQueueTail = chanPtr->inQueueTail;
	chanPtr->inQueueHead = NULL;
	chanPtr->inQueueTail = NULL;
	return 0;
    }

    /*
     * Nothing in the pushback area, fall back to the usual handling (driver,
     * etc.)
     */

    /*
     * See if we can fill an existing buffer. If we can, read only as much as
     * will fit in it. Otherwise allocate a new buffer, add it to the input
     * queue and attempt to fill it to the max.
     */

    bufPtr = statePtr->inQueueTail;
    if ((bufPtr != NULL) && !IsBufferFull(bufPtr)) {
	toRead = SpaceLeft(bufPtr);
    } else {
	bufPtr = statePtr->saveInBufPtr;
	statePtr->saveInBufPtr = NULL;

	/*
	 * Check the actual buffersize against the requested buffersize.
	 * Buffers which are smaller than requested are squashed. This is done
	 * to honor dynamic changes of the buffersize made by the user.
	 */

	if ((bufPtr != NULL)
		&& (bufPtr->bufLength - BUFFER_PADDING < statePtr->bufSize)) {
	    ckfree((char *) bufPtr);
	    bufPtr = NULL;
	}

	if (bufPtr == NULL) {
	    bufPtr = AllocChannelBuffer(statePtr->bufSize);
	}
	bufPtr->nextPtr = NULL;

	/*
	 * SF #427196: Use the actual size of the buffer to determine the
	 * number of bytes to read from the channel and not the size for new
	 * buffers. They can be different if the buffersize was changed
	 * between reads.
	 *
	 * Note: This affects performance negatively if the buffersize was
	 * extended but this small buffer is reused for all subsequent reads.
	 * The system never uses buffers with the requested bigger size in
	 * that case. An adjunct patch could try and delete all unused buffers
	 * it encounters and which are smaller than the formally requested
	 * buffersize.
	 */

	toRead = SpaceLeft(bufPtr);

	if (statePtr->inQueueTail == NULL) {
	    statePtr->inQueueHead = bufPtr;
	} else {
	    statePtr->inQueueTail->nextPtr = bufPtr;
	}
	statePtr->inQueueTail = bufPtr;
    }

    /*
     * If EOF is set, we should avoid calling the driver because on some
     * platforms it is impossible to read from a device after EOF.
     */

    if (statePtr->flags & CHANNEL_EOF) {
	return 0;
    }

#ifdef TCL_IO_TRACK_OS_FOR_DRIVER_WITH_BAD_BLOCKING
    /*
     * [SF Tcl Bug 943274]. Better emulation of non-blocking channels for
     * channels without BlockModeProc, by keeping track of true fileevents
     * generated by the OS == Data waiting and reading if and only if we are
     * sure to have data.
     */

    if ((statePtr->flags & CHANNEL_NONBLOCKING) &&
	    (Tcl_ChannelBlockModeProc(chanPtr->typePtr) == NULL) &&
	    !(statePtr->flags & CHANNEL_HAS_MORE_DATA)) {
	/*
	 * Bypass the driver, it would block, as no data is available
	 */

	nread = -1;
	result = EWOULDBLOCK;
    } else {
#endif /* TCL_IO_TRACK_OS_FOR_DRIVER_WITH_BAD_BLOCKING */

	nread = (chanPtr->typePtr->inputProc)(chanPtr->instanceData,
		InsertPoint(bufPtr), toRead, &result);

#ifdef TCL_IO_TRACK_OS_FOR_DRIVER_WITH_BAD_BLOCKING
    }
#endif /* TCL_IO_TRACK_OS_FOR_DRIVER_WITH_BAD_BLOCKING */

    if (nread > 0) {
	bufPtr->nextAdded += nread;

	/*
	 * If we get a short read, signal up that we may be BLOCKED. We should
	 * avoid calling the driver because on some platforms we will block in
	 * the low level reading code even though the channel is set into
	 * nonblocking mode.
	 */

	if (nread < toRead) {
	    SetFlag(statePtr, CHANNEL_BLOCKED);
	}

#ifdef TCL_IO_TRACK_OS_FOR_DRIVER_WITH_BAD_BLOCKING
	if (nread <= toRead) {
	    /*
	     * [SF Tcl Bug 943274] We have read the available data, clear
	     * flag.
	     */

	    ResetFlag(statePtr, CHANNEL_HAS_MORE_DATA);
	}
#endif /* TCL_IO_TRACK_OS_FOR_DRIVER_WITH_BAD_BLOCKING */

    } else if (nread == 0) {
	SetFlag(statePtr, CHANNEL_EOF);
	statePtr->inputEncodingFlags |= TCL_ENCODING_END;
    } else if (nread < 0) {
	if ((result == EWOULDBLOCK) || (result == EAGAIN)) {
	    SetFlag(statePtr, CHANNEL_BLOCKED);
	    result = EAGAIN;
	}
	Tcl_SetErrno(result);
	return result;
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_Seek --
 *
 *	Implements seeking on Tcl Channels. This is a public function so that
 *	other C facilities may be implemented on top of it.
 *
 * Results:
 *	The new access point or -1 on error. If error, use Tcl_GetErrno() to
 *	retrieve the POSIX error code for the error that occurred.
 *
 * Side effects:
 *	May flush output on the channel. May discard queued input.
 *
 *----------------------------------------------------------------------
 */

Tcl_WideInt
Tcl_Seek(
    Tcl_Channel chan,		/* The channel on which to seek. */
    Tcl_WideInt offset,		/* Offset to seek to. */
    int mode)			/* Relative to which location to seek? */
{
    Channel *chanPtr = (Channel *) chan;
				/* The real IO channel. */
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    int inputBuffered, outputBuffered;
				/* # bytes held in buffers. */
    int result;			/* Of device driver operations. */
    Tcl_WideInt curPos;		/* Position on the device. */
    int wasAsync;		/* Was the channel nonblocking before the seek
				 * operation? If so, must restore to
				 * non-blocking mode after the seek. */

    if (CheckChannelErrors(statePtr, TCL_WRITABLE | TCL_READABLE) != 0) {
	return Tcl_LongAsWide(-1);
    }

    /*
     * Disallow seek on dead channels - channels that have been closed but not
     * yet been deallocated. Such channels can be found if the exit handler
     * for channel cleanup has run but the channel is still registered in an
     * interpreter.
     */

    if (CheckForDeadChannel(NULL, statePtr)) {
	return Tcl_LongAsWide(-1);
    }

    /*
     * This operation should occur at the top of a channel stack.
     */

    chanPtr = statePtr->topChanPtr;

    /*
     * Disallow seek on channels whose type does not have a seek procedure
     * defined. This means that the channel does not support seeking.
     */

    if (chanPtr->typePtr->seekProc == NULL) {
	Tcl_SetErrno(EINVAL);
	return Tcl_LongAsWide(-1);
    }

    /*
     * Compute how much input and output is buffered. If both input and output
     * is buffered, cannot compute the current position.
     */

    inputBuffered = Tcl_InputBuffered(chan);
    outputBuffered = Tcl_OutputBuffered(chan);

    if ((inputBuffered != 0) && (outputBuffered != 0)) {
	Tcl_SetErrno(EFAULT);
	return Tcl_LongAsWide(-1);
    }

    /*
     * If we are seeking relative to the current position, compute the
     * corrected offset taking into account the amount of unread input.
     */

    if (mode == SEEK_CUR) {
	offset -= inputBuffered;
    }

    /*
     * Discard any queued input - this input should not be read after the
     * seek.
     */

    DiscardInputQueued(statePtr, 0);

    /*
     * Reset EOF and BLOCKED flags. We invalidate them by moving the access
     * point. Also clear CR related flags.
     */

    statePtr->flags &=
	~(CHANNEL_EOF | CHANNEL_STICKY_EOF | CHANNEL_BLOCKED | INPUT_SAW_CR);

    /*
     * If the channel is in asynchronous output mode, switch it back to
     * synchronous mode and cancel any async flush that may be scheduled.
     * After the flush, the channel will be put back into asynchronous output
     * mode.
     */

    wasAsync = 0;
    if (statePtr->flags & CHANNEL_NONBLOCKING) {
	wasAsync = 1;
	result = StackSetBlockMode(chanPtr, TCL_MODE_BLOCKING);
	if (result != 0) {
	    return Tcl_LongAsWide(-1);
	}
	ResetFlag(statePtr, CHANNEL_NONBLOCKING);
	if (statePtr->flags & BG_FLUSH_SCHEDULED) {
	    ResetFlag(statePtr, BG_FLUSH_SCHEDULED);
	}
    }

    /*
     * If there is data buffered in statePtr->curOutPtr then mark the channel
     * as ready to flush before invoking FlushChannel.
     */

    if ((statePtr->curOutPtr != NULL) && IsBufferReady(statePtr->curOutPtr)) {
	SetFlag(statePtr, BUFFER_READY);
    }

    /*
     * If the flush fails we cannot recover the original position. In that
     * case the seek is not attempted because we do not know where the access
     * position is - instead we return the error. FlushChannel has already
     * called Tcl_SetErrno() to report the error upwards. If the flush
     * succeeds we do the seek also.
     */

    if (FlushChannel(NULL, chanPtr, 0) != 0) {
	curPos = -1;
    } else {
	/*
	 * Now seek to the new position in the channel as requested by the
	 * caller. Note that we prefer the wideSeekProc if that is available
	 * and non-NULL...
	 */

	if (HaveVersion(chanPtr->typePtr, TCL_CHANNEL_VERSION_3) &&
		chanPtr->typePtr->wideSeekProc != NULL) {
	    curPos = (chanPtr->typePtr->wideSeekProc) (chanPtr->instanceData,
		    offset, mode, &result);
	} else if (offset < Tcl_LongAsWide(LONG_MIN) ||
		offset > Tcl_LongAsWide(LONG_MAX)) {
	    result = EOVERFLOW;
	    curPos = Tcl_LongAsWide(-1);
	} else {
	    curPos = Tcl_LongAsWide((chanPtr->typePtr->seekProc) (
		    chanPtr->instanceData, Tcl_WideAsLong(offset), mode,
		    &result));
	}
	if (curPos == Tcl_LongAsWide(-1)) {
	    Tcl_SetErrno(result);
	}
    }

    /*
     * Restore to nonblocking mode if that was the previous behavior.
     *
     * NOTE: Even if there was an async flush active we do not restore it now
     * because we already flushed all the queued output, above.
     */

    if (wasAsync) {
	SetFlag(statePtr, CHANNEL_NONBLOCKING);
	result = StackSetBlockMode(chanPtr, TCL_MODE_NONBLOCKING);
	if (result != 0) {
	    return Tcl_LongAsWide(-1);
	}
    }

    return curPos;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_Tell --
 *
 *	Returns the position of the next character to be read/written on this
 *	channel.
 *
 * Results:
 *	A nonnegative integer on success, -1 on failure. If failed, use
 *	Tcl_GetErrno() to retrieve the POSIX error code for the error that
 *	occurred.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_WideInt
Tcl_Tell(
    Tcl_Channel chan)		/* The channel to return pos for. */
{
    Channel *chanPtr = (Channel *) chan;
				/* The real IO channel. */
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    int inputBuffered, outputBuffered;
				/* # bytes held in buffers. */
    int result;			/* Of calling device driver. */
    Tcl_WideInt curPos;		/* Position on device. */

    if (CheckChannelErrors(statePtr, TCL_WRITABLE | TCL_READABLE) != 0) {
	return Tcl_LongAsWide(-1);
    }

    /*
     * Disallow tell on dead channels -- channels that have been closed but
     * not yet been deallocated. Such channels can be found if the exit
     * handler for channel cleanup has run but the channel is still registered
     * in an interpreter.
     */

    if (CheckForDeadChannel(NULL, statePtr)) {
	return Tcl_LongAsWide(-1);
    }

    /*
     * This operation should occur at the top of a channel stack.
     */

    chanPtr = statePtr->topChanPtr;

    /*
     * Disallow tell on channels whose type does not have a seek procedure
     * defined. This means that the channel does not support seeking.
     */

    if (chanPtr->typePtr->seekProc == NULL) {
	Tcl_SetErrno(EINVAL);
	return Tcl_LongAsWide(-1);
    }

    /*
     * Compute how much input and output is buffered. If both input and output
     * is buffered, cannot compute the current position.
     */

    inputBuffered = Tcl_InputBuffered(chan);
    outputBuffered = Tcl_OutputBuffered(chan);

    if ((inputBuffered != 0) && (outputBuffered != 0)) {
	Tcl_SetErrno(EFAULT);
	return Tcl_LongAsWide(-1);
    }

    /*
     * Get the current position in the device and compute the position where
     * the next character will be read or written. Note that we prefer the
     * wideSeekProc if that is available and non-NULL...
     */

    if (HaveVersion(chanPtr->typePtr, TCL_CHANNEL_VERSION_3) &&
	    chanPtr->typePtr->wideSeekProc != NULL) {
	curPos = (chanPtr->typePtr->wideSeekProc) (chanPtr->instanceData,
		Tcl_LongAsWide(0), SEEK_CUR, &result);
    } else {
	curPos = Tcl_LongAsWide((chanPtr->typePtr->seekProc) (
		chanPtr->instanceData, 0, SEEK_CUR, &result));
    }
    if (curPos == Tcl_LongAsWide(-1)) {
	Tcl_SetErrno(result);
	return Tcl_LongAsWide(-1);
    }
    if (inputBuffered != 0) {
	return curPos - inputBuffered;
    }
    return curPos + outputBuffered;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_SeekOld, Tcl_TellOld --
 *
 *	Backward-compatability versions of the seek/tell interface that do not
 *	support 64-bit offsets. This interface is not documented or expected
 *	to be supported indefinitely.
 *
 * Results:
 *	As for Tcl_Seek and Tcl_Tell respectively, except truncated to
 *	whatever value will fit in an 'int'.
 *
 * Side effects:
 *	As for Tcl_Seek and Tcl_Tell respectively.
 *
 *---------------------------------------------------------------------------
 */

int
Tcl_SeekOld(
    Tcl_Channel chan,		/* The channel on which to seek. */
    int offset,			/* Offset to seek to. */
    int mode)			/* Relative to which location to seek? */
{
    Tcl_WideInt wOffset, wResult;

    wOffset = Tcl_LongAsWide((long)offset);
    wResult = Tcl_Seek(chan, wOffset, mode);
    return (int)Tcl_WideAsLong(wResult);
}

int
Tcl_TellOld(
    Tcl_Channel chan)		/* The channel to return pos for. */
{
    Tcl_WideInt wResult;

    wResult = Tcl_Tell(chan);
    return (int)Tcl_WideAsLong(wResult);
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_TruncateChannel --
 *
 *	Truncate a channel to the given length.
 *
 * Results:
 *	TCL_OK on success, TCL_ERROR if the operation failed (e.g. is not
 *	supported by the type of channel, or the underlying OS operation
 *	failed in some way).
 *
 * Side effects:
 *	Seeks the channel to the current location. Sets errno on OS error.
 *
 *---------------------------------------------------------------------------
 */

int
Tcl_TruncateChannel(
    Tcl_Channel chan,		/* Channel to truncate. */
    Tcl_WideInt length)		/* Length to truncate it to. */
{
    Channel *chanPtr = (Channel *) chan;
    Tcl_DriverTruncateProc *truncateProc =
	    Tcl_ChannelTruncateProc(chanPtr->typePtr);
    int result;

    if (truncateProc == NULL) {
	/*
	 * Feature not supported and it's not emulatable. Pretend it's
	 * returned an EINVAL, a very generic error!
	 */

	Tcl_SetErrno(EINVAL);
	return TCL_ERROR;
    }

    if (!(chanPtr->state->flags & TCL_WRITABLE)) {
	/*
	 * We require that the file was opened of writing. Do that check now
	 * so that we only flush if we think we're going to succeed.
	 */

	Tcl_SetErrno(EINVAL);
	return TCL_ERROR;
    }

    /*
     * Seek first to force a total flush of all pending buffers and ditch any
     * pre-read input data.
     */

    if (Tcl_Seek(chan, (Tcl_WideInt)0, SEEK_CUR) == Tcl_LongAsWide(-1)) {
	return TCL_ERROR;
    }

    /*
     * We're all flushed to disk now and we also don't have any unfortunate
     * input baggage around either; can truncate with impunity.
     */

    result = truncateProc(chanPtr->instanceData, length);
    if (result != 0) {
	Tcl_SetErrno(result);
	return TCL_ERROR;
    }
    return TCL_OK;
}

/*
 *---------------------------------------------------------------------------
 *
 * CheckChannelErrors --
 *
 *	See if the channel is in an ready state and can perform the desired
 *	operation.
 *
 * Results:
 *	The return value is 0 if the channel is OK, otherwise the return value
 *	is -1 and errno is set to indicate the error.
 *
 * Side effects:
 *	May clear the EOF and/or BLOCKED bits if reading from channel.
 *
 *---------------------------------------------------------------------------
 */

static int
CheckChannelErrors(
    ChannelState *statePtr,	/* Channel to check. */
    int flags)			/* Test if channel supports desired operation:
				 * TCL_READABLE, TCL_WRITABLE. Also indicates
				 * Raw read or write for special close
				 * processing */
{
    int direction = flags & (TCL_READABLE|TCL_WRITABLE);

    /*
     * Check for unreported error.
     */

    if (statePtr->unreportedError != 0) {
	Tcl_SetErrno(statePtr->unreportedError);
	statePtr->unreportedError = 0;

	/*
	 * TIP #219, Tcl Channel Reflection API.
	 * Move a defered error message back into the channel bypass.
	 */

	if (statePtr->chanMsg != NULL) {
	    TclDecrRefCount(statePtr->chanMsg);
	}
	statePtr->chanMsg = statePtr->unreportedMsg;
	statePtr->unreportedMsg = NULL;
	return -1;
    }

    /*
     * Only the raw read and write operations are allowed during close in
     * order to drain data from stacked channels.
     */

    if ((statePtr->flags & CHANNEL_CLOSED) &&
	    ((flags & CHANNEL_RAW_MODE) == 0)) {
	Tcl_SetErrno(EACCES);
	return -1;
    }

    /*
     * Fail if the channel is not opened for desired operation.
     */

    if ((statePtr->flags & direction) == 0) {
	Tcl_SetErrno(EACCES);
	return -1;
    }

    /*
     * Fail if the channel is in the middle of a background copy.
     *
     * Don't do this tests for raw channels here or else the chaining in the
     * transformation drivers will fail with 'file busy' error instead of
     * retrieving and transforming the data to copy.
     */

    if (BUSY_STATE(statePtr,flags) && ((flags & CHANNEL_RAW_MODE) == 0)) {
	Tcl_SetErrno(EBUSY);
	return -1;
    }

    if (direction == TCL_READABLE) {
	/*
	 * If we have not encountered a sticky EOF, clear the EOF bit (sticky
	 * EOF is set if we have seen the input eofChar, to prevent reading
	 * beyond the eofChar). Also, always clear the BLOCKED bit. We want to
	 * discover these conditions anew in each operation.
	 */

	if ((statePtr->flags & CHANNEL_STICKY_EOF) == 0) {
	    ResetFlag(statePtr, CHANNEL_EOF);
	}
	ResetFlag(statePtr, CHANNEL_BLOCKED | CHANNEL_NEED_MORE_DATA);
    }

    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_Eof --
 *
 *	Returns 1 if the channel is at EOF, 0 otherwise.
 *
 * Results:
 *	1 or 0, always.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_Eof(
    Tcl_Channel chan)		/* Does this channel have EOF? */
{
    ChannelState *statePtr = ((Channel *) chan)->state;
				/* State of real channel structure. */

    return ((statePtr->flags & CHANNEL_STICKY_EOF) ||
	    ((statePtr->flags & CHANNEL_EOF) &&
	    (Tcl_InputBuffered(chan) == 0))) ? 1 : 0;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_InputBlocked --
 *
 *	Returns 1 if input is blocked on this channel, 0 otherwise.
 *
 * Results:
 *	0 or 1, always.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_InputBlocked(
    Tcl_Channel chan)		/* Is this channel blocked? */
{
    ChannelState *statePtr = ((Channel *) chan)->state;
				/* State of real channel structure. */

    return (statePtr->flags & CHANNEL_BLOCKED) ? 1 : 0;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_InputBuffered --
 *
 *	Returns the number of bytes of input currently buffered in the common
 *	internal buffer of a channel.
 *
 * Results:
 *	The number of input bytes buffered, or zero if the channel is not open
 *	for reading.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_InputBuffered(
    Tcl_Channel chan)		/* The channel to query. */
{
    ChannelState *statePtr = ((Channel *) chan)->state;
				/* State of real channel structure. */
    ChannelBuffer *bufPtr;
    int bytesBuffered;

    for (bytesBuffered = 0, bufPtr = statePtr->inQueueHead; bufPtr != NULL;
	    bufPtr = bufPtr->nextPtr) {
	bytesBuffered += BytesLeft(bufPtr);
    }

    /*
     * Don't forget the bytes in the topmost pushback area.
     */

    for (bufPtr = statePtr->topChanPtr->inQueueHead; bufPtr != NULL;
	    bufPtr = bufPtr->nextPtr) {
	bytesBuffered += BytesLeft(bufPtr);
    }

    return bytesBuffered;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_OutputBuffered --
 *
 *    Returns the number of bytes of output currently buffered in the common
 *    internal buffer of a channel.
 *
 * Results:
 *    The number of output bytes buffered, or zero if the channel is not open
 *    for writing.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_OutputBuffered(
    Tcl_Channel chan)		/* The channel to query. */
{
    ChannelState *statePtr = ((Channel *) chan)->state;
				/* State of real channel structure. */
    ChannelBuffer *bufPtr;
    int bytesBuffered;

    for (bytesBuffered = 0, bufPtr = statePtr->outQueueHead; bufPtr != NULL;
	    bufPtr = bufPtr->nextPtr) {
	bytesBuffered += BytesLeft(bufPtr);
    }
    if (statePtr->curOutPtr != NULL) {
	register ChannelBuffer *curOutPtr = statePtr->curOutPtr;

	if (IsBufferReady(curOutPtr)) {
	    bytesBuffered += BytesLeft(curOutPtr);
	}
    }

    return bytesBuffered;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ChannelBuffered --
 *
 *	Returns the number of bytes of input currently buffered in the
 *	internal buffer (push back area) of a channel.
 *
 * Results:
 *	The number of input bytes buffered, or zero if the channel is not open
 *	for reading.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_ChannelBuffered(
    Tcl_Channel chan)		/* The channel to query. */
{
    Channel *chanPtr = (Channel *) chan;
				/* Real channel structure. */
    ChannelBuffer *bufPtr;
    int bytesBuffered = 0;

    for (bufPtr = chanPtr->inQueueHead; bufPtr != NULL;
	    bufPtr = bufPtr->nextPtr) {
	bytesBuffered += BytesLeft(bufPtr);
    }

    return bytesBuffered;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetChannelBufferSize --
 *
 *	Sets the size of buffers to allocate to store input or output in the
 *	channel. The size must be between 1 byte and 1 MByte.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Sets the size of buffers subsequently allocated for this channel.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_SetChannelBufferSize(
    Tcl_Channel chan,		/* The channel whose buffer size to set. */
    int sz)			/* The size to set. */
{
    ChannelState *statePtr;	/* State of real channel structure. */

    /*
     * If the buffer size is smaller than 1 byte or larger than one MByte, do
     * not accept the requested size and leave the current buffer size.
     */

    if (sz < 1 || sz > 1024*1024) {
	return;
    }

    statePtr = ((Channel *) chan)->state;
    statePtr->bufSize = sz;

    if (statePtr->outputStage != NULL) {
	ckfree((char *) statePtr->outputStage);
	statePtr->outputStage = NULL;
    }
    if ((statePtr->encoding != NULL) && (statePtr->flags & TCL_WRITABLE)) {
	statePtr->outputStage = (char *)
		ckalloc((unsigned) (statePtr->bufSize + 2));
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetChannelBufferSize --
 *
 *	Retrieves the size of buffers to allocate for this channel.
 *
 * Results:
 *	The size.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_GetChannelBufferSize(
    Tcl_Channel chan)		/* The channel for which to find the buffer
				 * size. */
{
    ChannelState *statePtr = ((Channel *) chan)->state;
				/* State of real channel structure. */

    return statePtr->bufSize;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_BadChannelOption --
 *
 *	This procedure generates a "bad option" error message in an (optional)
 *	interpreter. It is used by channel drivers when a invalid Set/Get
 *	option is requested. Its purpose is to concatenate the generic options
 *	list to the specific ones and factorize the generic options error
 *	message string.
 *
 * Results:
 *	TCL_ERROR.
 *
 * Side effects:

 *	An error message is generated in interp's result object to indicate
 *	that a command was invoked with the a bad option. The message has the
 *	form:
 *		bad option "blah": should be one of
 *		<...generic options...>+<...specific options...>
 *	"blah" is the optionName argument and "<specific options>" is a space
 *	separated list of specific option words. The function takes good care
 *	of inserting minus signs before each option, commas after, and an "or"
 *	before the last option.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_BadChannelOption(
    Tcl_Interp *interp,		/* Current interpreter (can be NULL).*/
    const char *optionName,	/* 'bad option' name */
    const char *optionList)	/* Specific options list to append to the
				 * standard generic options. Can be NULL for
				 * generic options only. */
{
    if (interp != NULL) {
	const char *genericopt =
		"blocking buffering buffersize encoding eofchar translation";
	const char **argv;
	int argc, i;
	Tcl_DString ds;

	Tcl_DStringInit(&ds);
	Tcl_DStringAppend(&ds, genericopt, -1);
	if (optionList && (*optionList)) {
	    Tcl_DStringAppend(&ds, " ", 1);
	    Tcl_DStringAppend(&ds, optionList, -1);
	}
	if (Tcl_SplitList(interp, Tcl_DStringValue(&ds),
		&argc, &argv) != TCL_OK) {
	    Tcl_Panic("malformed option list in channel driver");
	}
	Tcl_ResetResult(interp);
	Tcl_AppendResult(interp, "bad option \"", optionName,
		"\": should be one of ", NULL);
	argc--;
	for (i = 0; i < argc; i++) {
	    Tcl_AppendResult(interp, "-", argv[i], ", ", NULL);
	}
	Tcl_AppendResult(interp, "or -", argv[i], NULL);
	Tcl_DStringFree(&ds);
	ckfree((char *) argv);
    }
    Tcl_SetErrno(EINVAL);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetChannelOption --
 *
 *	Gets a mode associated with an IO channel. If the optionName arg is
 *	non NULL, retrieves the value of that option. If the optionName arg is
 *	NULL, retrieves a list of alternating option names and values for the
 *	given channel.
 *
 * Results:
 *	A standard Tcl result. Also sets the supplied DString to the string
 *	value of the option(s) returned.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_GetChannelOption(
    Tcl_Interp *interp,		/* For error reporting - can be NULL. */
    Tcl_Channel chan,		/* Channel on which to get option. */
    const char *optionName,	/* Option to get. */
    Tcl_DString *dsPtr)		/* Where to store value(s). */
{
    size_t len;			/* Length of optionName string. */
    char optionVal[128];	/* Buffer for sprintf. */
    Channel *chanPtr = (Channel *) chan;
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    int flags;

    /*
     * Disallow options on dead channels -- channels that have been closed but
     * not yet been deallocated. Such channels can be found if the exit
     * handler for channel cleanup has run but the channel is still registered
     * in an interpreter.
     */

    if (CheckForDeadChannel(interp, statePtr)) {
	return TCL_ERROR;
    }

    /*
     * This operation should occur at the top of a channel stack.
     */

    chanPtr = statePtr->topChanPtr;

    /*
     * If we are in the middle of a background copy, use the saved flags.
     */

    if (statePtr->csPtrR) {
      flags = statePtr->csPtrR->readFlags;
    } else if (statePtr->csPtrW) {
      flags = statePtr->csPtrW->writeFlags;
    } else {
	flags = statePtr->flags;
    }

    /*
     * If the optionName is NULL it means that we want a list of all options
     * and values.
     */

    if (optionName == NULL) {
	len = 0;
    } else {
	len = strlen(optionName);
    }

    if (len == 0 || HaveOpt(2, "-blocking")) {
	if (len == 0) {
	    Tcl_DStringAppendElement(dsPtr, "-blocking");
	}
	Tcl_DStringAppendElement(dsPtr,
		(flags & CHANNEL_NONBLOCKING) ? "0" : "1");
	if (len > 0) {
	    return TCL_OK;
	}
    }
    if (len == 0 || HaveOpt(7, "-buffering")) {
	if (len == 0) {
	    Tcl_DStringAppendElement(dsPtr, "-buffering");
	}
	if (flags & CHANNEL_LINEBUFFERED) {
	    Tcl_DStringAppendElement(dsPtr, "line");
	} else if (flags & CHANNEL_UNBUFFERED) {
	    Tcl_DStringAppendElement(dsPtr, "none");
	} else {
	    Tcl_DStringAppendElement(dsPtr, "full");
	}
	if (len > 0) {
	    return TCL_OK;
	}
    }
    if (len == 0 || HaveOpt(7, "-buffersize")) {
	if (len == 0) {
	    Tcl_DStringAppendElement(dsPtr, "-buffersize");
	}
	TclFormatInt(optionVal, statePtr->bufSize);
	Tcl_DStringAppendElement(dsPtr, optionVal);
	if (len > 0) {
	    return TCL_OK;
	}
    }
    if (len == 0 || HaveOpt(2, "-encoding")) {
	if (len == 0) {
	    Tcl_DStringAppendElement(dsPtr, "-encoding");
	}
	if (statePtr->encoding == NULL) {
	    Tcl_DStringAppendElement(dsPtr, "binary");
	} else {
	    Tcl_DStringAppendElement(dsPtr,
		    Tcl_GetEncodingName(statePtr->encoding));
	}
	if (len > 0) {
	    return TCL_OK;
	}
    }
    if (len == 0 || HaveOpt(2, "-eofchar")) {
	if (len == 0) {
	    Tcl_DStringAppendElement(dsPtr, "-eofchar");
	}
	if (((flags & (TCL_READABLE|TCL_WRITABLE)) ==
		(TCL_READABLE|TCL_WRITABLE)) && (len == 0)) {
	    Tcl_DStringStartSublist(dsPtr);
	}
	if (flags & TCL_READABLE) {
	    if (statePtr->inEofChar == 0) {
		Tcl_DStringAppendElement(dsPtr, "");
	    } else {
		char buf[4];

		sprintf(buf, "%c", statePtr->inEofChar);
		Tcl_DStringAppendElement(dsPtr, buf);
	    }
	}
	if (flags & TCL_WRITABLE) {
	    if (statePtr->outEofChar == 0) {
		Tcl_DStringAppendElement(dsPtr, "");
	    } else {
		char buf[4];

		sprintf(buf, "%c", statePtr->outEofChar);
		Tcl_DStringAppendElement(dsPtr, buf);
	    }
	}
	if (!(flags & (TCL_READABLE|TCL_WRITABLE))) {
	    /*
	     * Not readable or writable (e.g. server socket)
	     */

	    Tcl_DStringAppendElement(dsPtr, "");
	}
	if (((flags & (TCL_READABLE|TCL_WRITABLE)) ==
		(TCL_READABLE|TCL_WRITABLE)) && (len == 0)) {
	    Tcl_DStringEndSublist(dsPtr);
	}
	if (len > 0) {
	    return TCL_OK;
	}
    }
    if (len == 0 || HaveOpt(1, "-translation")) {
	if (len == 0) {
	    Tcl_DStringAppendElement(dsPtr, "-translation");
	}
	if (((flags & (TCL_READABLE|TCL_WRITABLE)) ==
		(TCL_READABLE|TCL_WRITABLE)) && (len == 0)) {
	    Tcl_DStringStartSublist(dsPtr);
	}
	if (flags & TCL_READABLE) {
	    if (statePtr->inputTranslation == TCL_TRANSLATE_AUTO) {
		Tcl_DStringAppendElement(dsPtr, "auto");
	    } else if (statePtr->inputTranslation == TCL_TRANSLATE_CR) {
		Tcl_DStringAppendElement(dsPtr, "cr");
	    } else if (statePtr->inputTranslation == TCL_TRANSLATE_CRLF) {
		Tcl_DStringAppendElement(dsPtr, "crlf");
	    } else {
		Tcl_DStringAppendElement(dsPtr, "lf");
	    }
	}
	if (flags & TCL_WRITABLE) {
	    if (statePtr->outputTranslation == TCL_TRANSLATE_AUTO) {
		Tcl_DStringAppendElement(dsPtr, "auto");
	    } else if (statePtr->outputTranslation == TCL_TRANSLATE_CR) {
		Tcl_DStringAppendElement(dsPtr, "cr");
	    } else if (statePtr->outputTranslation == TCL_TRANSLATE_CRLF) {
		Tcl_DStringAppendElement(dsPtr, "crlf");
	    } else {
		Tcl_DStringAppendElement(dsPtr, "lf");
	    }
	}
	if (!(flags & (TCL_READABLE|TCL_WRITABLE))) {
	    /*
	     * Not readable or writable (e.g. server socket)
	     */

	    Tcl_DStringAppendElement(dsPtr, "auto");
	}
	if (((flags & (TCL_READABLE|TCL_WRITABLE)) ==
		(TCL_READABLE|TCL_WRITABLE)) && (len == 0)) {
	    Tcl_DStringEndSublist(dsPtr);
	}
	if (len > 0) {
	    return TCL_OK;
	}
    }

    if (chanPtr->typePtr->getOptionProc != NULL) {
	/*
	 * Let the driver specific handle additional options and result code
	 * and message.
	 */

	return (chanPtr->typePtr->getOptionProc) (chanPtr->instanceData,
		interp, optionName, dsPtr);
    } else {
	/*
	 * No driver specific options case.
	 */

	if (len == 0) {
	    return TCL_OK;
	}
	return Tcl_BadChannelOption(interp, optionName, NULL);
    }
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_SetChannelOption --
 *
 *	Sets an option on a channel.
 *
 * Results:
 *	A standard Tcl result. On error, sets interp's result object if
 *	interp is not NULL.
 *
 * Side effects:
 *	May modify an option on a device.
 *
 *---------------------------------------------------------------------------
 */

int
Tcl_SetChannelOption(
    Tcl_Interp *interp,		/* For error reporting - can be NULL. */
    Tcl_Channel chan,		/* Channel on which to set mode. */
    const char *optionName,	/* Which option to set? */
    const char *newValue)	/* New value for option. */
{
    Channel *chanPtr = (Channel *) chan;
				/* The real IO channel. */
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    size_t len;			/* Length of optionName string. */
    int argc;
    const char **argv;

    /*
     * If the channel is in the middle of a background copy, fail.
     */

    if (statePtr->csPtrR || statePtr->csPtrW) {
	if (interp) {
	    Tcl_AppendResult(interp, "unable to set channel options: "
		    "background copy in progress", NULL);
	}
	return TCL_ERROR;
    }

    /*
     * Disallow options on dead channels -- channels that have been closed but
     * not yet been deallocated. Such channels can be found if the exit
     * handler for channel cleanup has run but the channel is still registered
     * in an interpreter.
     */

    if (CheckForDeadChannel(NULL, statePtr)) {
	return TCL_ERROR;
    }

    /*
     * This operation should occur at the top of a channel stack.
     */

    chanPtr = statePtr->topChanPtr;

    len = strlen(optionName);

    if (HaveOpt(2, "-blocking")) {
	int newMode;

	if (Tcl_GetBoolean(interp, newValue, &newMode) == TCL_ERROR) {
	    return TCL_ERROR;
	}
	if (newMode) {
	    newMode = TCL_MODE_BLOCKING;
	} else {
	    newMode = TCL_MODE_NONBLOCKING;
	}
	return SetBlockMode(interp, chanPtr, newMode);
    } else if (HaveOpt(7, "-buffering")) {
	len = strlen(newValue);
	if ((newValue[0] == 'f') && (strncmp(newValue, "full", len) == 0)) {
	    statePtr->flags &=
		    ~(CHANNEL_UNBUFFERED|CHANNEL_LINEBUFFERED);
	} else if ((newValue[0] == 'l') &&
		(strncmp(newValue, "line", len) == 0)) {
	    ResetFlag(statePtr, CHANNEL_UNBUFFERED);
	    SetFlag(statePtr, CHANNEL_LINEBUFFERED);
	} else if ((newValue[0] == 'n') &&
		(strncmp(newValue, "none", len) == 0)) {
	    ResetFlag(statePtr, CHANNEL_LINEBUFFERED);
	    SetFlag(statePtr, CHANNEL_UNBUFFERED);
	} else {
	    if (interp) {
		Tcl_AppendResult(interp, "bad value for -buffering: "
			"must be one of full, line, or none", NULL);
		return TCL_ERROR;
	    }
	}
	return TCL_OK;
    } else if (HaveOpt(7, "-buffersize")) {
	int newBufferSize;

	if (Tcl_GetInt(interp, newValue, &newBufferSize) == TCL_ERROR) {
	    return TCL_ERROR;
	}
	Tcl_SetChannelBufferSize(chan, newBufferSize);
    } else if (HaveOpt(2, "-encoding")) {
	Tcl_Encoding encoding;

	if ((newValue[0] == '\0') || (strcmp(newValue, "binary") == 0)) {
	    encoding = NULL;
	} else {
	    encoding = Tcl_GetEncoding(interp, newValue);
	    if (encoding == NULL) {
		return TCL_ERROR;
	    }
	}

	/*
	 * When the channel has an escape sequence driven encoding such as
	 * iso2022, the terminated escape sequence must write to the buffer.
	 */

	if ((statePtr->encoding != NULL) && (statePtr->curOutPtr != NULL)
		&& (CheckChannelErrors(statePtr, TCL_WRITABLE) == 0)) {
	    statePtr->outputEncodingFlags |= TCL_ENCODING_END;
	    WriteChars(chanPtr, "", 0);
	}
	Tcl_FreeEncoding(statePtr->encoding);
	statePtr->encoding = encoding;
	statePtr->inputEncodingState = NULL;
	statePtr->inputEncodingFlags = TCL_ENCODING_START;
	statePtr->outputEncodingState = NULL;
	statePtr->outputEncodingFlags = TCL_ENCODING_START;
	ResetFlag(statePtr, CHANNEL_NEED_MORE_DATA);
	UpdateInterest(chanPtr);
    } else if (HaveOpt(2, "-eofchar")) {
	if (Tcl_SplitList(interp, newValue, &argc, &argv) == TCL_ERROR) {
	    return TCL_ERROR;
	}
	if (argc == 0) {
	    statePtr->inEofChar = 0;
	    statePtr->outEofChar = 0;
	} else if (argc == 1 || argc == 2) {
	    int outIndex = (argc - 1);
	    int inValue = (int) argv[0][0];
	    int outValue = (int) argv[outIndex][0];
	    if (inValue & 0x80 || outValue & 0x80) {
		if (interp) {
		    Tcl_AppendResult(interp, "bad value for -eofchar: ",
			    "must be non-NUL ASCII character", NULL);
		}
		ckfree((char *) argv);
		return TCL_ERROR;
	    }
	    if (statePtr->flags & TCL_READABLE) {
		statePtr->inEofChar = inValue;
	    }
	    if (statePtr->flags & TCL_WRITABLE) {
		statePtr->outEofChar = outValue;
	    }
	} else {
	    if (interp) {
		Tcl_AppendResult(interp,
			"bad value for -eofchar: should be a list of zero,"
			" one, or two elements", NULL);
	    }
	    ckfree((char *) argv);
	    return TCL_ERROR;
	}
	if (argv != NULL) {
	    ckfree((char *) argv);
	}

	/*
	 * [Bug 930851] Reset EOF and BLOCKED flags. Changing the character
	 * which signals eof can transform a current eof condition into a 'go
	 * ahead'. Ditto for blocked.
	 */

	statePtr->flags &=
		~(CHANNEL_EOF | CHANNEL_STICKY_EOF | CHANNEL_BLOCKED);

	return TCL_OK;
    } else if (HaveOpt(1, "-translation")) {
	const char *readMode, *writeMode;

	if (Tcl_SplitList(interp, newValue, &argc, &argv) == TCL_ERROR) {
	    return TCL_ERROR;
	}

	if (argc == 1) {
	    readMode = (statePtr->flags & TCL_READABLE) ? argv[0] : NULL;
	    writeMode = (statePtr->flags & TCL_WRITABLE) ? argv[0] : NULL;
	} else if (argc == 2) {
	    readMode = (statePtr->flags & TCL_READABLE) ? argv[0] : NULL;
	    writeMode = (statePtr->flags & TCL_WRITABLE) ? argv[1] : NULL;
	} else {
	    if (interp) {
		Tcl_AppendResult(interp,
			"bad value for -translation: must be a one or two"
			" element list", NULL);
	    }
	    ckfree((char *) argv);
	    return TCL_ERROR;
	}

	if (readMode) {
	    TclEolTranslation translation;
	    if (*readMode == '\0') {
		translation = statePtr->inputTranslation;
	    } else if (strcmp(readMode, "auto") == 0) {
		translation = TCL_TRANSLATE_AUTO;
	    } else if (strcmp(readMode, "binary") == 0) {
		translation = TCL_TRANSLATE_LF;
		statePtr->inEofChar = 0;
		Tcl_FreeEncoding(statePtr->encoding);
		statePtr->encoding = NULL;
	    } else if (strcmp(readMode, "lf") == 0) {
		translation = TCL_TRANSLATE_LF;
	    } else if (strcmp(readMode, "cr") == 0) {
		translation = TCL_TRANSLATE_CR;
	    } else if (strcmp(readMode, "crlf") == 0) {
		translation = TCL_TRANSLATE_CRLF;
	    } else if (strcmp(readMode, "platform") == 0) {
		translation = TCL_PLATFORM_TRANSLATION;
	    } else {
		if (interp) {
		    Tcl_AppendResult(interp,
			    "bad value for -translation: "
			    "must be one of auto, binary, cr, lf, crlf,"
			    " or platform", NULL);
		}
		ckfree((char *) argv);
		return TCL_ERROR;
	    }

	    /*
	     * Reset the EOL flags since we need to look at any buffered data
	     * to see if the new translation mode allows us to complete the
	     * line.
	     */

	    if (translation != statePtr->inputTranslation) {
		statePtr->inputTranslation = translation;
		ResetFlag(statePtr, INPUT_SAW_CR | CHANNEL_NEED_MORE_DATA);
		UpdateInterest(chanPtr);
	    }
	}
	if (writeMode) {
	    if (*writeMode == '\0') {
		/* Do nothing. */
	    } else if (strcmp(writeMode, "auto") == 0) {
		/*
		 * This is a hack to get TCP sockets to produce output in CRLF
		 * mode if they are being set into AUTO mode. A better
		 * solution for achieving this effect will be coded later.
		 */

		if (strcmp(Tcl_ChannelName(chanPtr->typePtr), "tcp") == 0) {
		    statePtr->outputTranslation = TCL_TRANSLATE_CRLF;
		} else {
		    statePtr->outputTranslation = TCL_PLATFORM_TRANSLATION;
		}
	    } else if (strcmp(writeMode, "binary") == 0) {
		statePtr->outEofChar = 0;
		statePtr->outputTranslation = TCL_TRANSLATE_LF;
		Tcl_FreeEncoding(statePtr->encoding);
		statePtr->encoding = NULL;
	    } else if (strcmp(writeMode, "lf") == 0) {
		statePtr->outputTranslation = TCL_TRANSLATE_LF;
	    } else if (strcmp(writeMode, "cr") == 0) {
		statePtr->outputTranslation = TCL_TRANSLATE_CR;
	    } else if (strcmp(writeMode, "crlf") == 0) {
		statePtr->outputTranslation = TCL_TRANSLATE_CRLF;
	    } else if (strcmp(writeMode, "platform") == 0) {
		statePtr->outputTranslation = TCL_PLATFORM_TRANSLATION;
	    } else {
		if (interp) {
		    Tcl_AppendResult(interp,
			    "bad value for -translation: "
			    "must be one of auto, binary, cr, lf, crlf,"
			    " or platform", NULL);
		}
		ckfree((char *) argv);
		return TCL_ERROR;
	    }
	}
	ckfree((char *) argv);
	return TCL_OK;
    } else if (chanPtr->typePtr->setOptionProc != NULL) {
	return (*chanPtr->typePtr->setOptionProc)(chanPtr->instanceData,
		interp, optionName, newValue);
    } else {
	return Tcl_BadChannelOption(interp, optionName, NULL);
    }

    /*
     * If bufsize changes, need to get rid of old utility buffer.
     */

    if (statePtr->saveInBufPtr != NULL) {
	RecycleBuffer(statePtr, statePtr->saveInBufPtr, 1);
	statePtr->saveInBufPtr = NULL;
    }
    if ((statePtr->inQueueHead != NULL)
	    && (statePtr->inQueueHead->nextPtr == NULL)
	    && IsBufferEmpty(statePtr->inQueueHead)) {
	RecycleBuffer(statePtr, statePtr->inQueueHead, 1);
	statePtr->inQueueHead = NULL;
	statePtr->inQueueTail = NULL;
    }

    /*
     * If encoding or bufsize changes, need to update output staging buffer.
     */

    if (statePtr->outputStage != NULL) {
	ckfree(statePtr->outputStage);
	statePtr->outputStage = NULL;
    }
    if ((statePtr->encoding != NULL) && (statePtr->flags & TCL_WRITABLE)) {
	statePtr->outputStage = ckalloc((unsigned) (statePtr->bufSize + 2));
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * CleanupChannelHandlers --
 *
 *	Removes channel handlers that refer to the supplied interpreter, so
 *	that if the actual channel is not closed now, these handlers will not
 *	run on subsequent events on the channel. This would be erroneous,
 *	because the interpreter no longer has a reference to this channel.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Removes channel handlers.
 *
 *----------------------------------------------------------------------
 */

static void
CleanupChannelHandlers(
    Tcl_Interp *interp,
    Channel *chanPtr)
{
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    EventScriptRecord *sPtr, *prevPtr, *nextPtr;

    /*
     * Remove fileevent records on this channel that refer to the given
     * interpreter.
     */

    for (sPtr = statePtr->scriptRecordPtr, prevPtr = NULL;
	    sPtr != NULL; sPtr = nextPtr) {
	nextPtr = sPtr->nextPtr;
	if (sPtr->interp == interp) {
	    if (prevPtr == NULL) {
		statePtr->scriptRecordPtr = nextPtr;
	    } else {
		prevPtr->nextPtr = nextPtr;
	    }

	    Tcl_DeleteChannelHandler((Tcl_Channel) chanPtr,
		    TclChannelEventScriptInvoker, sPtr);

	    TclDecrRefCount(sPtr->scriptPtr);
	    ckfree((char *) sPtr);
	} else {
	    prevPtr = sPtr;
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_NotifyChannel --
 *
 *	This procedure is called by a channel driver when a driver detects an
 *	event on a channel. This procedure is responsible for actually
 *	handling the event by invoking any channel handler callbacks.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Whatever the channel handler callback procedure does.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_NotifyChannel(
    Tcl_Channel channel,	/* Channel that detected an event. */
    int mask)			/* OR'ed combination of TCL_READABLE,
				 * TCL_WRITABLE, or TCL_EXCEPTION: indicates
				 * which events were detected. */
{
    Channel *chanPtr = (Channel *) channel;
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    ChannelHandler *chPtr;
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    NextChannelHandler nh;
    Channel *upChanPtr;
    const Tcl_ChannelType *upTypePtr;

#ifdef TCL_IO_TRACK_OS_FOR_DRIVER_WITH_BAD_BLOCKING
    /*
     * [SF Tcl Bug 943274] For a non-blocking channel without blockmodeproc we
     * keep track of actual input coming from the OS so that we can do a
     * credible imitation of non-blocking behaviour.
     */

    if ((mask & TCL_READABLE) &&
	    (statePtr->flags & CHANNEL_NONBLOCKING) &&
	    (Tcl_ChannelBlockModeProc(chanPtr->typePtr) == NULL) &&
	    !(statePtr->flags & CHANNEL_TIMER_FEV)) {
	SetFlag(statePtr, CHANNEL_HAS_MORE_DATA);
    }
#endif /* TCL_IO_TRACK_OS_FOR_DRIVER_WITH_BAD_BLOCKING */

    /*
     * In contrast to the other API functions this procedure walks towards the
     * top of a stack and not down from it.
     *
     * The channel calling this procedure is the one who generated the event,
     * and thus does not take part in handling it. IOW, its HandlerProc is not
     * called, instead we begin with the channel above it.
     *
     * This behaviour also allows the transformation channels to generate
     * their own events and pass them upward.
     */

    while (mask && (chanPtr->upChanPtr != (NULL))) {
	Tcl_DriverHandlerProc *upHandlerProc;

	upChanPtr = chanPtr->upChanPtr;
	upTypePtr = upChanPtr->typePtr;
	upHandlerProc = Tcl_ChannelHandlerProc(upTypePtr);
	if (upHandlerProc != NULL) {
	    mask = (*upHandlerProc) (upChanPtr->instanceData, mask);
	}

	/*
	 * ELSE: Ignore transformations which are unable to handle the event
	 * coming from below. Assume that they don't change the mask and pass
	 * it on.
	 */

	chanPtr = upChanPtr;
    }

    channel = (Tcl_Channel) chanPtr;

    /*
     * Here we have either reached the top of the stack or the mask is empty.
     * We break out of the procedure if it is the latter.
     */

    if (!mask) {
	return;
    }

    /*
     * We are now above the topmost channel in a stack and have events left.
     * Now call the channel handlers as usual.
     *
     * Preserve the channel struct in case the script closes it.
     */

    Tcl_Preserve(channel);
    Tcl_Preserve(statePtr);

    /*
     * If we are flushing in the background, be sure to call FlushChannel for
     * writable events. Note that we have to discard the writable event so we
     * don't call any write handlers before the flush is complete.
     */

    if ((statePtr->flags & BG_FLUSH_SCHEDULED) && (mask & TCL_WRITABLE)) {
	FlushChannel(NULL, chanPtr, 1);
	mask &= ~TCL_WRITABLE;
    }

    /*
     * Add this invocation to the list of recursive invocations of
     * ChannelHandlerEventProc.
     */

    nh.nextHandlerPtr = NULL;
    nh.nestedHandlerPtr = tsdPtr->nestedHandlerPtr;
    tsdPtr->nestedHandlerPtr = &nh;

    for (chPtr = statePtr->chPtr; chPtr != NULL; ) {
	/*
	 * If this channel handler is interested in any of the events that
	 * have occurred on the channel, invoke its procedure.
	 */

	if ((chPtr->mask & mask) != 0) {
	    nh.nextHandlerPtr = chPtr->nextPtr;
	    (*(chPtr->proc))(chPtr->clientData, mask);
	    chPtr = nh.nextHandlerPtr;
	} else {
	    chPtr = chPtr->nextPtr;
	}
    }

    /*
     * Update the notifier interest, since it may have changed after invoking
     * event handlers. Skip that if the channel was deleted in the call to the
     * channel handler.
     */

    if (chanPtr->typePtr != NULL) {
	UpdateInterest(chanPtr);
    }

    Tcl_Release(statePtr);
    Tcl_Release(channel);

    tsdPtr->nestedHandlerPtr = nh.nestedHandlerPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * UpdateInterest --
 *
 *	Arrange for the notifier to call us back at appropriate times based on
 *	the current state of the channel.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	May schedule a timer or driver handler.
 *
 *----------------------------------------------------------------------
 */

static void
UpdateInterest(
    Channel *chanPtr)		/* Channel to update. */
{
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    int mask = statePtr->interestMask;

    /*
     * If there are flushed buffers waiting to be written, then we need to
     * watch for the channel to become writable.
     */

    if (statePtr->flags & BG_FLUSH_SCHEDULED) {
	mask |= TCL_WRITABLE;
    }

    /*
     * If there is data in the input queue, and we aren't waiting for more
     * data, then we need to schedule a timer so we don't block in the
     * notifier. Also, cancel the read interest so we don't get duplicate
     * events.
     */

    if (mask & TCL_READABLE) {
	if (!(statePtr->flags & CHANNEL_NEED_MORE_DATA)
		&& (statePtr->inQueueHead != NULL)
		&& IsBufferReady(statePtr->inQueueHead)) {
	    mask &= ~TCL_READABLE;

	    /*
	     * Andreas Kupries, April 11, 2003
	     *
	     * Some operating systems (Solaris 2.6 and higher (but not Solaris
	     * 2.5, go figure)) generate READABLE and EXCEPTION events when
	     * select()'ing [*] on a plain file, even if EOF was not yet
	     * reached. This is a problem in the following situation:
	     *
	     * - An extension asks to get both READABLE and EXCEPTION events.
	     * - It reads data into a buffer smaller than the buffer used by
	     *	 Tcl itself.
	     * - It does not process all events in the event queue, but only
	     *	 one, at least in some situations.
	     *
	     * In that case we can get into a situation where
	     *
	     * - Tcl drops READABLE here, because it has data in its own
	     *	 buffers waiting to be read by the extension.
	     * - A READABLE event is syntesized via timer.
	     * - The OS still reports the EXCEPTION condition on the file.
	     * - And the extension gets the EXCPTION event first, and handles
	     *	 this as EOF.
	     *
	     * End result ==> Premature end of reading from a file.
	     *
	     * The concrete example is 'Expect', and its [expect] command
	     * (and at the C-level, deep in the bowels of Expect,
	     * 'exp_get_next_event'. See marker 'SunOS' for commentary in
	     * that function too).
	     *
	     * [*] As the Tcl notifier does. See also for marker 'SunOS' in
	     * file 'exp_event.c' of Expect.
	     *
	     * Our solution here is to drop the interest in the EXCEPTION
	     * events too. This compiles on all platforms, and also passes the
	     * testsuite on all of them.
	     */

	    mask &= ~TCL_EXCEPTION;

	    if (!statePtr->timer) {
		statePtr->timer = Tcl_CreateTimerHandler(0, ChannelTimerProc,
			chanPtr);
	    }
	}
    }
    (chanPtr->typePtr->watchProc)(chanPtr->instanceData, mask);
}

/*
 *----------------------------------------------------------------------
 *
 * ChannelTimerProc --
 *
 *	Timer handler scheduled by UpdateInterest to monitor the channel
 *	buffers until they are empty.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	May invoke channel handlers.
 *
 *----------------------------------------------------------------------
 */

static void
ChannelTimerProc(
    ClientData clientData)
{
    Channel *chanPtr = clientData;
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */

    if (!(statePtr->flags & CHANNEL_NEED_MORE_DATA)
	    && (statePtr->interestMask & TCL_READABLE)
	    && (statePtr->inQueueHead != NULL)
	    && IsBufferReady(statePtr->inQueueHead)) {
	/*
	 * Restart the timer in case a channel handler reenters the event loop
	 * before UpdateInterest gets called by Tcl_NotifyChannel.
	 */

	statePtr->timer = Tcl_CreateTimerHandler(0, ChannelTimerProc,chanPtr);

#ifdef TCL_IO_TRACK_OS_FOR_DRIVER_WITH_BAD_BLOCKING
	/*
	 * Set the TIMER flag to notify the higher levels that the driver
	 * might have no data for us. We do this only if we are in
	 * non-blocking mode and the driver has no BlockModeProc because only
	 * then we really don't know if the driver will block or not. A
	 * similar test is done in "PeekAhead".
	 */

	if ((statePtr->flags & CHANNEL_NONBLOCKING) &&
	    (Tcl_ChannelBlockModeProc(chanPtr->typePtr) == NULL)) {
	    SetFlag(statePtr, CHANNEL_TIMER_FEV);
	}
#endif /* TCL_IO_TRACK_OS_FOR_DRIVER_WITH_BAD_BLOCKING */

	Tcl_Preserve(statePtr);
	Tcl_NotifyChannel((Tcl_Channel)chanPtr, TCL_READABLE);

#ifdef TCL_IO_TRACK_OS_FOR_DRIVER_WITH_BAD_BLOCKING
	ResetFlag(statePtr, CHANNEL_TIMER_FEV);
#endif /* TCL_IO_TRACK_OS_FOR_DRIVER_WITH_BAD_BLOCKING */

	Tcl_Release(statePtr);
    } else {
	statePtr->timer = NULL;
	UpdateInterest(chanPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_CreateChannelHandler --
 *
 *	Arrange for a given procedure to be invoked whenever the channel
 *	indicated by the chanPtr arg becomes readable or writable.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	From now on, whenever the I/O channel given by chanPtr becomes ready
 *	in the way indicated by mask, proc will be invoked. See the manual
 *	entry for details on the calling sequence to proc. If there is already
 *	an event handler for chan, proc and clientData, then the mask will be
 *	updated.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_CreateChannelHandler(
    Tcl_Channel chan,		/* The channel to create the handler for. */
    int mask,			/* OR'ed combination of TCL_READABLE,
				 * TCL_WRITABLE, and TCL_EXCEPTION: indicates
				 * conditions under which proc should be
				 * called. Use 0 to disable a registered
				 * handler. */
    Tcl_ChannelProc *proc,	/* Procedure to call for each selected
				 * event. */
    ClientData clientData)	/* Arbitrary data to pass to proc. */
{
    ChannelHandler *chPtr;
    Channel *chanPtr = (Channel *) chan;
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */

    /*
     * Check whether this channel handler is not already registered. If it is
     * not, create a new record, else reuse existing record (smash current
     * values).
     */

    for (chPtr = statePtr->chPtr; chPtr != NULL; chPtr = chPtr->nextPtr) {
	if ((chPtr->chanPtr == chanPtr) && (chPtr->proc == proc) &&
		(chPtr->clientData == clientData)) {
	    break;
	}
    }
    if (chPtr == NULL) {
	chPtr = (ChannelHandler *) ckalloc(sizeof(ChannelHandler));
	chPtr->mask = 0;
	chPtr->proc = proc;
	chPtr->clientData = clientData;
	chPtr->chanPtr = chanPtr;
	chPtr->nextPtr = statePtr->chPtr;
	statePtr->chPtr = chPtr;
    }

    /*
     * The remainder of the initialization below is done regardless of whether
     * or not this is a new record or a modification of an old one.
     */

    chPtr->mask = mask;

    /*
     * Recompute the interest mask for the channel - this call may actually be
     * disabling an existing handler.
     */

    statePtr->interestMask = 0;
    for (chPtr = statePtr->chPtr; chPtr != NULL; chPtr = chPtr->nextPtr) {
	statePtr->interestMask |= chPtr->mask;
    }

    UpdateInterest(statePtr->topChanPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_DeleteChannelHandler --
 *
 *	Cancel a previously arranged callback arrangement for an IO channel.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	If a callback was previously registered for this chan, proc and
 *	clientData, it is removed and the callback will no longer be called
 *	when the channel becomes ready for IO.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_DeleteChannelHandler(
    Tcl_Channel chan,		/* The channel for which to remove the
				 * callback. */
    Tcl_ChannelProc *proc,	/* The procedure in the callback to delete. */
    ClientData clientData)	/* The client data in the callback to
				 * delete. */
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    ChannelHandler *chPtr, *prevChPtr;
    Channel *chanPtr = (Channel *) chan;
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    NextChannelHandler *nhPtr;

    /*
     * Find the entry and the previous one in the list.
     */

    for (prevChPtr = NULL, chPtr = statePtr->chPtr; chPtr != NULL;
	    chPtr = chPtr->nextPtr) {
	if ((chPtr->chanPtr == chanPtr) && (chPtr->clientData == clientData)
		&& (chPtr->proc == proc)) {
	    break;
	}
	prevChPtr = chPtr;
    }

    /*
     * If not found, return without doing anything.
     */

    if (chPtr == NULL) {
	return;
    }

    /*
     * If ChannelHandlerEventProc is about to process this handler, tell it to
     * process the next one instead - we are going to delete *this* one.
     */

    for (nhPtr = tsdPtr->nestedHandlerPtr; nhPtr != NULL;
	    nhPtr = nhPtr->nestedHandlerPtr) {
	if (nhPtr->nextHandlerPtr == chPtr) {
	    nhPtr->nextHandlerPtr = chPtr->nextPtr;
	}
    }

    /*
     * Splice it out of the list of channel handlers.
     */

    if (prevChPtr == NULL) {
	statePtr->chPtr = chPtr->nextPtr;
    } else {
	prevChPtr->nextPtr = chPtr->nextPtr;
    }
    ckfree((char *) chPtr);

    /*
     * Recompute the interest list for the channel, so that infinite loops
     * will not result if Tcl_DeleteChannelHandler is called inside an event.
     */

    statePtr->interestMask = 0;
    for (chPtr = statePtr->chPtr; chPtr != NULL; chPtr = chPtr->nextPtr) {
	statePtr->interestMask |= chPtr->mask;
    }

    UpdateInterest(statePtr->topChanPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * DeleteScriptRecord --
 *
 *	Delete a script record for this combination of channel, interp and
 *	mask.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Deletes a script record and cancels a channel event handler.
 *
 *----------------------------------------------------------------------
 */

static void
DeleteScriptRecord(
    Tcl_Interp *interp,		/* Interpreter in which script was to be
				 * executed. */
    Channel *chanPtr,		/* The channel for which to delete the script
				 * record (if any). */
    int mask)			/* Events in mask must exactly match mask of
				 * script to delete. */
{
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    EventScriptRecord *esPtr, *prevEsPtr;

    for (esPtr = statePtr->scriptRecordPtr, prevEsPtr = NULL; esPtr != NULL;
	    prevEsPtr = esPtr, esPtr = esPtr->nextPtr) {
	if ((esPtr->interp == interp) && (esPtr->mask == mask)) {
	    if (esPtr == statePtr->scriptRecordPtr) {
		statePtr->scriptRecordPtr = esPtr->nextPtr;
	    } else {
		prevEsPtr->nextPtr = esPtr->nextPtr;
	    }

	    Tcl_DeleteChannelHandler((Tcl_Channel) chanPtr,
		    TclChannelEventScriptInvoker, esPtr);

	    TclDecrRefCount(esPtr->scriptPtr);
	    ckfree((char *) esPtr);

	    break;
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * CreateScriptRecord --
 *
 *	Creates a record to store a script to be executed when a specific
 *	event fires on a specific channel.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Causes the script to be stored for later execution.
 *
 *----------------------------------------------------------------------
 */

static void
CreateScriptRecord(
    Tcl_Interp *interp,		/* Interpreter in which to execute the stored
				 * script. */
    Channel *chanPtr,		/* Channel for which script is to be stored */
    int mask,			/* Set of events for which script will be
				 * invoked. */
    Tcl_Obj *scriptPtr)		/* Pointer to script object. */
{
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    EventScriptRecord *esPtr;

    for (esPtr=statePtr->scriptRecordPtr; esPtr!=NULL; esPtr=esPtr->nextPtr) {
	if ((esPtr->interp == interp) && (esPtr->mask == mask)) {
	    TclDecrRefCount(esPtr->scriptPtr);
	    esPtr->scriptPtr = NULL;
	    break;
	}
    }
    if (esPtr == NULL) {
	esPtr = (EventScriptRecord *) ckalloc(sizeof(EventScriptRecord));
	Tcl_CreateChannelHandler((Tcl_Channel) chanPtr, mask,
		TclChannelEventScriptInvoker, esPtr);
	esPtr->nextPtr = statePtr->scriptRecordPtr;
	statePtr->scriptRecordPtr = esPtr;
    }
    esPtr->chanPtr = chanPtr;
    esPtr->interp = interp;
    esPtr->mask = mask;
    Tcl_IncrRefCount(scriptPtr);
    esPtr->scriptPtr = scriptPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TclChannelEventScriptInvoker --
 *
 *	Invokes a script scheduled by "fileevent" for when the channel becomes
 *	ready for IO. This function is invoked by the channel handler which
 *	was created by the Tcl "fileevent" command.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Whatever the script does.
 *
 *----------------------------------------------------------------------
 */

void
TclChannelEventScriptInvoker(
    ClientData clientData,	/* The script+interp record. */
    int mask)			/* Not used. */
{
    Tcl_Interp *interp;		/* Interpreter in which to eval the script. */
    Channel *chanPtr;		/* The channel for which this handler is
				 * registered. */
    EventScriptRecord *esPtr;	/* The event script + interpreter to eval it
				 * in. */
    int result;			/* Result of call to eval script. */

    esPtr = clientData;
    chanPtr = esPtr->chanPtr;
    mask = esPtr->mask;
    interp = esPtr->interp;

    /*
     * We must preserve the interpreter so we can report errors on it later.
     * Note that we do not need to preserve the channel because that is done
     * by Tcl_NotifyChannel before calling channel handlers.
     */

    Tcl_Preserve(interp);
    result = Tcl_EvalObjEx(interp, esPtr->scriptPtr, TCL_EVAL_GLOBAL);

    /*
     * On error, cause a background error and remove the channel handler and
     * the script record.
     *
     * NOTE: Must delete channel handler before causing the background error
     * because the background error may want to reinstall the handler.
     */

    if (result != TCL_OK) {
	if (chanPtr->typePtr != NULL) {
	    DeleteScriptRecord(interp, chanPtr, mask);
	}
	TclBackgroundException(interp, result);
    }
    Tcl_Release(interp);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FileEventObjCmd --
 *
 *	This procedure implements the "fileevent" Tcl command. See the user
 *	documentation for details on what it does. This command is based on
 *	the Tk command "fileevent" which in turn is based on work contributed
 *	by Mark Diekhans.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	May create a channel handler for the specified channel.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
Tcl_FileEventObjCmd(
    ClientData clientData,	/* Not used. */
    Tcl_Interp *interp,		/* Interpreter in which the channel for which
				 * to create the handler is found. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    Channel *chanPtr;		/* The channel to create the handler for. */
    ChannelState *statePtr;	/* State info for channel */
    Tcl_Channel chan;		/* The opaque type for the channel. */
    char *chanName;
    int modeIndex;		/* Index of mode argument. */
    int mask;
    static const char *modeOptions[] = {"readable", "writable", NULL};
    static int maskArray[] = {TCL_READABLE, TCL_WRITABLE};

    if ((objc != 3) && (objc != 4)) {
	Tcl_WrongNumArgs(interp, 1, objv, "channelId event ?script?");
	return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[2], modeOptions, "event name", 0,
	    &modeIndex) != TCL_OK) {
	return TCL_ERROR;
    }
    mask = maskArray[modeIndex];

    chanName = TclGetString(objv[1]);
    chan = Tcl_GetChannel(interp, chanName, NULL);
    if (chan == NULL) {
	return TCL_ERROR;
    }
    chanPtr = (Channel *) chan;
    statePtr = chanPtr->state;
    if ((statePtr->flags & mask) == 0) {
	Tcl_AppendResult(interp, "channel is not ",
		(mask == TCL_READABLE) ? "readable" : "writable", NULL);
	return TCL_ERROR;
    }

    /*
     * If we are supposed to return the script, do so.
     */

    if (objc == 3) {
	EventScriptRecord *esPtr;
	for (esPtr = statePtr->scriptRecordPtr; esPtr != NULL;
		esPtr = esPtr->nextPtr) {
	    if ((esPtr->interp == interp) && (esPtr->mask == mask)) {
		Tcl_SetObjResult(interp, esPtr->scriptPtr);
		break;
	    }
	}
	return TCL_OK;
    }

    /*
     * If we are supposed to delete a stored script, do so.
     */

    if (*(TclGetString(objv[3])) == '\0') {
	DeleteScriptRecord(interp, chanPtr, mask);
	return TCL_OK;
    }

    /*
     * Make the script record that will link between the event and the script
     * to invoke. This also creates a channel event handler which will
     * evaluate the script in the supplied interpreter.
     */

    CreateScriptRecord(interp, chanPtr, mask, objv[3]);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCopyChannel --
 *
 *	This routine copies data from one channel to another, either
 *	synchronously or asynchronously. If a command script is supplied, the
 *	operation runs in the background. The script is invoked when the copy
 *	completes. Otherwise the function waits until the copy is completed
 *	before returning.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	May schedule a background copy operation that causes both channels to
 *	be marked busy.
 *
 *----------------------------------------------------------------------
 */

int
TclCopyChannel(
    Tcl_Interp *interp,		/* Current interpreter. */
    Tcl_Channel inChan,		/* Channel to read from. */
    Tcl_Channel outChan,	/* Channel to write to. */
    int toRead,			/* Amount of data to copy, or -1 for all. */
    Tcl_Obj *cmdPtr)		/* Pointer to script to execute or NULL. */
{
    Channel *inPtr = (Channel *) inChan;
    Channel *outPtr = (Channel *) outChan;
    ChannelState *inStatePtr, *outStatePtr;
    int readFlags, writeFlags;
    CopyState *csPtr;
    int nonBlocking = (cmdPtr) ? CHANNEL_NONBLOCKING : 0;

    inStatePtr = inPtr->state;
    outStatePtr = outPtr->state;

    if (BUSY_STATE(inStatePtr,TCL_READABLE)) {
	if (interp) {
	    Tcl_AppendResult(interp, "channel \"",
		    Tcl_GetChannelName(inChan), "\" is busy", NULL);
	}
	return TCL_ERROR;
    }
    if (BUSY_STATE(outStatePtr,TCL_WRITABLE)) {
	if (interp) {
	    Tcl_AppendResult(interp, "channel \"",
		    Tcl_GetChannelName(outChan), "\" is busy", NULL);
	}
	return TCL_ERROR;
    }

    readFlags = inStatePtr->flags;
    writeFlags = outStatePtr->flags;

    /*
     * Set up the blocking mode appropriately. Background copies need
     * non-blocking channels. Foreground copies need blocking channels. If
     * there is an error, restore the old blocking mode.
     */

    if (nonBlocking != (readFlags & CHANNEL_NONBLOCKING)) {
	if (SetBlockMode(interp, inPtr, nonBlocking ?
		TCL_MODE_NONBLOCKING : TCL_MODE_BLOCKING) != TCL_OK) {
	    return TCL_ERROR;
	}
    }
    if ((inPtr!=outPtr) && (nonBlocking!=(writeFlags&CHANNEL_NONBLOCKING)) &&
	    (SetBlockMode(NULL, outPtr, nonBlocking ?
		    TCL_MODE_NONBLOCKING : TCL_MODE_BLOCKING) != TCL_OK) &&
	    (nonBlocking != (readFlags & CHANNEL_NONBLOCKING))) {
	SetBlockMode(NULL, inPtr, (readFlags & CHANNEL_NONBLOCKING)
		? TCL_MODE_NONBLOCKING : TCL_MODE_BLOCKING);
	return TCL_ERROR;
    }

    /*
     * Make sure the output side is unbuffered.
     */

    outStatePtr->flags = (outStatePtr->flags & ~(CHANNEL_LINEBUFFERED))
	| CHANNEL_UNBUFFERED;

    /*
     * Allocate a new CopyState to maintain info about the current copy in
     * progress. This structure will be deallocated when the copy is
     * completed.
     */

    csPtr = (CopyState *) ckalloc(sizeof(CopyState) + inStatePtr->bufSize);
    csPtr->bufSize = inStatePtr->bufSize;
    csPtr->readPtr = inPtr;
    csPtr->writePtr = outPtr;
    csPtr->readFlags = readFlags;
    csPtr->writeFlags = writeFlags;
    csPtr->toRead = toRead;
    csPtr->total = 0;
    csPtr->interp = interp;
    if (cmdPtr) {
	Tcl_IncrRefCount(cmdPtr);
    }
    csPtr->cmdPtr = cmdPtr;

    inStatePtr->csPtrR  = csPtr;
    outStatePtr->csPtrW = csPtr;

    /*
     * Start copying data between the channels.
     */

    return CopyData(csPtr, 0);
}

/*
 *----------------------------------------------------------------------
 *
 * CopyData --
 *
 *	This function implements the lowest level of the copying mechanism for
 *	TclCopyChannel.
 *
 * Results:
 *	Returns TCL_OK on success, else TCL_ERROR.
 *
 * Side effects:
 *	Moves data between channels, may create channel handlers.
 *
 *----------------------------------------------------------------------
 */

static int
CopyData(
    CopyState *csPtr,		/* State of copy operation. */
    int mask)			/* Current channel event flags. */
{
    Tcl_Interp *interp;
    Tcl_Obj *cmdPtr, *errObj = NULL, *bufObj = NULL, *msg = NULL;
    Tcl_Channel inChan, outChan;
    ChannelState *inStatePtr, *outStatePtr;
    int result = TCL_OK, size, total, sizeb;
    char *buffer;
    int inBinary, outBinary, sameEncoding;
				/* Encoding control */
    int underflow;		/* Input underflow */

    inChan	= (Tcl_Channel) csPtr->readPtr;
    outChan	= (Tcl_Channel) csPtr->writePtr;
    inStatePtr	= csPtr->readPtr->state;
    outStatePtr	= csPtr->writePtr->state;
    interp	= csPtr->interp;
    cmdPtr	= csPtr->cmdPtr;

    /*
     * Copy the data the slow way, using the translation mechanism.
     *
     * Note: We have make sure that we use the topmost channel in a stack for
     * the copying. The caller uses Tcl_GetChannel to access it, and thus gets
     * the bottom of the stack.
     */

    inBinary = (inStatePtr->encoding == NULL);
    outBinary = (outStatePtr->encoding == NULL);
    sameEncoding = (inStatePtr->encoding == outStatePtr->encoding);

    if (!(inBinary || sameEncoding)) {
	TclNewObj(bufObj);
	Tcl_IncrRefCount(bufObj);
    }

    while (csPtr->toRead != 0) {
	/*
	 * Check for unreported background errors.
	 */

	Tcl_GetChannelError(inChan, &msg);
	if ((inStatePtr->unreportedError != 0) || (msg != NULL)) {
	    Tcl_SetErrno(inStatePtr->unreportedError);
	    inStatePtr->unreportedError = 0;
	    goto readError;
	}
	Tcl_GetChannelError(outChan, &msg);
	if ((outStatePtr->unreportedError != 0) || (msg != NULL)) {
	    Tcl_SetErrno(outStatePtr->unreportedError);
	    outStatePtr->unreportedError = 0;
	    goto writeError;
	}

	if (cmdPtr && (mask == 0)) {
	    /*
	     * In async mode, we skip reading synchronously and fake an
	     * underflow instead to prime the readable fileevent.
	     */

	    size      = 0;
	    underflow = 1;
	} else {
	    /*
	     * Read up to bufSize bytes.
	     */

	    if ((csPtr->toRead == -1) || (csPtr->toRead > csPtr->bufSize)) {
		sizeb = csPtr->bufSize;
	    } else {
		sizeb = csPtr->toRead;
	    }

	    if (inBinary || sameEncoding) {
		size = DoRead(inStatePtr->topChanPtr, csPtr->buffer, sizeb);
	    } else {
		size = DoReadChars(inStatePtr->topChanPtr, bufObj, sizeb,
				   0 /* No append */);
	    }
	    underflow = (size >= 0) && (size < sizeb);	/* Input underflow */
	}

	if (size < 0) {
	readError:
	    if (interp) {
		TclNewObj(errObj);
		Tcl_AppendStringsToObj(errObj, "error reading \"",
			Tcl_GetChannelName(inChan), "\": ", NULL);
		if (msg != NULL) {
		    Tcl_AppendObjToObj(errObj, msg);
		} else {
		    Tcl_AppendStringsToObj(errObj, Tcl_PosixError(interp),
			    NULL);
		}
	    }
	    if (msg != NULL) {
		Tcl_DecrRefCount(msg);
	    }
	    break;
	} else if (underflow) {
	    /*
	     * We had an underflow on the read side. If we are at EOF, and not
	     * in the synchronous part of an asynchronous fcopy, then the
	     * copying is done, otherwise set up a channel handler to detect
	     * when the channel becomes readable again.
	     */

	    if ((size == 0) && Tcl_Eof(inChan) && !(cmdPtr && (mask == 0))) {
		break;
	    }
	    if (((!Tcl_Eof(inChan)) || (cmdPtr && (mask == 0))) &&
		!(mask & TCL_READABLE)) {
		if (mask & TCL_WRITABLE) {
		    Tcl_DeleteChannelHandler(outChan, CopyEventProc, csPtr);
		}
		Tcl_CreateChannelHandler(inChan, TCL_READABLE, CopyEventProc,
			csPtr);
	    }
	    if (size == 0) {
		if (bufObj != NULL) {
		    TclDecrRefCount(bufObj);
		    bufObj = NULL;
		}
		return TCL_OK;
	    }
	}

	/*
	 * Now write the buffer out.
	 */

	if (inBinary || sameEncoding) {
	    buffer = csPtr->buffer;
	    sizeb = size;
	} else {
	    buffer = TclGetStringFromObj(bufObj, &sizeb);
	}

	if (outBinary || sameEncoding) {
	    sizeb = DoWrite(outStatePtr->topChanPtr, buffer, sizeb);
	} else {
	    sizeb = DoWriteChars(outStatePtr->topChanPtr, buffer, sizeb);
	}

	if (inBinary || sameEncoding) {
	    /*
	     * Both read and write counted bytes.
	     */

	    size = sizeb;
	} /* else: Read counted characters, write counted bytes, i.e.
	   * size != sizeb */

	if (sizeb < 0) {
	writeError:
	    if (interp) {
		TclNewObj(errObj);
		Tcl_AppendStringsToObj(errObj, "error writing \"",
			Tcl_GetChannelName(outChan), "\": ", NULL);
		if (msg != NULL) {
		    Tcl_AppendObjToObj(errObj, msg);
		} else {
		    Tcl_AppendStringsToObj(errObj, Tcl_PosixError(interp),
			    NULL);
		}
	    }
	    if (msg != NULL) {
		Tcl_DecrRefCount(msg);
	    }
	    break;
	}

	/*
	 * Update the current byte count. Do it now so the count is valid
	 * before a return or break takes us out of the loop. The invariant at
	 * the top of the loop should be that csPtr->toRead holds the number
	 * of bytes left to copy.
	 */

	if (csPtr->toRead != -1) {
	    csPtr->toRead -= size;
	}
	csPtr->total += size;

	/*
	 * Break loop if EOF && (size>0)
	 */

	if (Tcl_Eof(inChan)) {
	    break;
	}

	/*
	 * Check to see if the write is happening in the background. If so,
	 * stop copying and wait for the channel to become writable again.
	 * After input underflow we already installed a readable handler
	 * therefore we don't need a writable handler.
	 */

	if (!underflow && (outStatePtr->flags & BG_FLUSH_SCHEDULED)) {
	    if (!(mask & TCL_WRITABLE)) {
		if (mask & TCL_READABLE) {
		    Tcl_DeleteChannelHandler(inChan, CopyEventProc, csPtr);
		}
		Tcl_CreateChannelHandler(outChan, TCL_WRITABLE,
			CopyEventProc, csPtr);
	    }
	    if (bufObj != NULL) {
		TclDecrRefCount(bufObj);
		bufObj = NULL;
	    }
	    return TCL_OK;
	}

	/*
	 * For background copies, we only do one buffer per invocation so we
	 * don't starve the rest of the system.
	 */

	if (cmdPtr && (csPtr->toRead != 0)) {
	    /*
	     * The first time we enter this code, there won't be a channel
	     * handler established yet, so do it here.
	     */

	    if (mask == 0) {
		Tcl_CreateChannelHandler(outChan, TCL_WRITABLE, CopyEventProc,
			csPtr);
	    }
	    if (bufObj != NULL) {
		TclDecrRefCount(bufObj);
		bufObj = NULL;
	    }
	    return TCL_OK;
	}
    } /* while */

    if (bufObj != NULL) {
	TclDecrRefCount(bufObj);
	bufObj = NULL;
    }

    /*
     * Make the callback or return the number of bytes transferred. The local
     * total is used because StopCopy frees csPtr.
     */

    total = csPtr->total;
    if (cmdPtr && interp) {
	int code;
	/*
	 * Get a private copy of the command so we can mutate it by adding
	 * arguments. Note that StopCopy frees our saved reference to the
	 * original command obj.
	 */

	cmdPtr = Tcl_DuplicateObj(cmdPtr);
	Tcl_IncrRefCount(cmdPtr);
	StopCopy(csPtr);
	Tcl_Preserve(interp);

	Tcl_ListObjAppendElement(interp, cmdPtr, Tcl_NewIntObj(total));
	if (errObj) {
	    Tcl_ListObjAppendElement(interp, cmdPtr, errObj);
	}
	code = Tcl_EvalObjEx(interp, cmdPtr, TCL_EVAL_GLOBAL);
	if (code != TCL_OK) {
	    TclBackgroundException(interp, code);
	    result = TCL_ERROR;
	}
	TclDecrRefCount(cmdPtr);
	Tcl_Release(interp);
    } else {
	StopCopy(csPtr);
	if (interp) {
	    if (errObj) {
		Tcl_SetObjResult(interp, errObj);
		result = TCL_ERROR;
	    } else {
		Tcl_ResetResult(interp);
		Tcl_SetObjResult(interp, Tcl_NewIntObj(total));
	    }
	}
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * DoRead --
 *
 *	Reads a given number of bytes from a channel.
 *
 *	No encoding conversions are applied to the bytes being read.
 *
 * Results:
 *	The number of characters read, or -1 on error. Use Tcl_GetErrno() to
 *	retrieve the error code for the error that occurred.
 *
 * Side effects:
 *	May cause input to be buffered.
 *
 *----------------------------------------------------------------------
 */

static int
DoRead(
    Channel *chanPtr,		/* The channel from which to read. */
    char *bufPtr,		/* Where to store input read. */
    int toRead)			/* Maximum number of bytes to read. */
{
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    int copied;			/* How many characters were copied into the
				 * result string? */
    int copiedNow;		/* How many characters were copied from the
				 * current input buffer? */
    int result;			/* Of calling GetInput. */

    /*
     * If we have not encountered a sticky EOF, clear the EOF bit. Either way
     * clear the BLOCKED bit. We want to discover these anew during each
     * operation.
     */

    if (!(statePtr->flags & CHANNEL_STICKY_EOF)) {
	ResetFlag(statePtr, CHANNEL_EOF);
    }
    ResetFlag(statePtr, CHANNEL_BLOCKED | CHANNEL_NEED_MORE_DATA);

    for (copied = 0; copied < toRead; copied += copiedNow) {
	copiedNow = CopyAndTranslateBuffer(statePtr, bufPtr + copied,
		toRead - copied);
	if (copiedNow == 0) {
	    if (statePtr->flags & CHANNEL_EOF) {
		goto done;
	    }
	    if (statePtr->flags & CHANNEL_BLOCKED) {
		if (statePtr->flags & CHANNEL_NONBLOCKING) {
		    goto done;
		}
		ResetFlag(statePtr, CHANNEL_BLOCKED);
	    }
	    result = GetInput(chanPtr);
	    if (result != 0) {
		if (result != EAGAIN) {
		    copied = -1;
		}
		goto done;
	    }
	}
    }

    ResetFlag(statePtr, CHANNEL_BLOCKED);

    /*
     * Update the notifier state so we don't block while there is still data
     * in the buffers.
     */

  done:
    UpdateInterest(chanPtr);
    return copied;
}

/*
 *----------------------------------------------------------------------
 *
 * CopyAndTranslateBuffer --
 *
 *	Copy at most one buffer of input to the result space, doing eol
 *	translations according to mode in effect currently.
 *
 * Results:
 *	Number of bytes stored in the result buffer (as opposed to the number
 *	of bytes read from the channel). May return zero if no input is
 *	available to be translated.
 *
 * Side effects:
 *	Consumes buffered input. May deallocate one buffer.
 *
 *----------------------------------------------------------------------
 */

static int
CopyAndTranslateBuffer(
    ChannelState *statePtr,	/* Channel state from which to read input. */
    char *result,		/* Where to store the copied input. */
    int space)			/* How many bytes are available in result to
				 * store the copied input? */
{
    ChannelBuffer *bufPtr;	/* The buffer from which to copy bytes. */
    int bytesInBuffer;		/* How many bytes are available to be copied
				 * in the current input buffer? */
    int copied;			/* How many characters were already copied
				 * into the destination space? */
    int i;			/* Iterates over the copied input looking for
				 * the input eofChar. */

    /*
     * If there is no input at all, return zero. The invariant is that either
     * there is no buffer in the queue, or if the first buffer is empty, it is
     * also the last buffer (and thus there is no input in the queue). Note
     * also that if the buffer is empty, we leave it in the queue.
     */

    if (statePtr->inQueueHead == NULL) {
	return 0;
    }
    bufPtr = statePtr->inQueueHead;
    bytesInBuffer = BytesLeft(bufPtr);

    copied = 0;
    switch (statePtr->inputTranslation) {
    case TCL_TRANSLATE_LF:
	if (bytesInBuffer == 0) {
	    return 0;
	}

	/*
	 * Copy the current chunk into the result buffer.
	 */

	if (bytesInBuffer < space) {
	    space = bytesInBuffer;
	}
	memcpy(result, RemovePoint(bufPtr), (size_t) space);
	bufPtr->nextRemoved += space;
	copied = space;
	break;
    case TCL_TRANSLATE_CR: {
	char *end;

	if (bytesInBuffer == 0) {
	    return 0;
	}

	/*
	 * Copy the current chunk into the result buffer, then replace all \r
	 * with \n.
	 */

	if (bytesInBuffer < space) {
	    space = bytesInBuffer;
	}
	memcpy(result, RemovePoint(bufPtr), (size_t) space);
	bufPtr->nextRemoved += space;
	copied = space;

	for (end = result + copied; result < end; result++) {
	    if (*result == '\r') {
		*result = '\n';
	    }
	}
	break;
    }
    case TCL_TRANSLATE_CRLF: {
	char *src, *end, *dst;
	int curByte;

	/*
	 * If there is a held-back "\r" at EOF, produce it now.
	 */

	if (bytesInBuffer == 0) {
	    if ((statePtr->flags & (INPUT_SAW_CR | CHANNEL_EOF)) ==
		    (INPUT_SAW_CR | CHANNEL_EOF)) {
		result[0] = '\r';
		ResetFlag(statePtr, INPUT_SAW_CR);
		return 1;
	    }
	    return 0;
	}

	/*
	 * Copy the current chunk and replace "\r\n" with "\n" (but not
	 * standalone "\r"!).
	 */

	if (bytesInBuffer < space) {
	    space = bytesInBuffer;
	}
	memcpy(result, RemovePoint(bufPtr), (size_t) space);
	bufPtr->nextRemoved += space;
	copied = space;

	end = result + copied;
	dst = result;
	for (src = result; src < end; src++) {
	    curByte = *src;
	    if (curByte == '\n') {
		ResetFlag(statePtr, INPUT_SAW_CR);
	    } else if (statePtr->flags & INPUT_SAW_CR) {
		ResetFlag(statePtr, INPUT_SAW_CR);
		*dst = '\r';
		dst++;
	    }
	    if (curByte == '\r') {
		SetFlag(statePtr, INPUT_SAW_CR);
	    } else {
		*dst = (char) curByte;
		dst++;
	    }
	}
	copied = dst - result;
	break;
    }
    case TCL_TRANSLATE_AUTO: {
	char *src, *end, *dst;
	int curByte;

	if (bytesInBuffer == 0) {
	    return 0;
	}

	/*
	 * Loop over the current buffer, converting "\r" and "\r\n" to "\n".
	 */

	if (bytesInBuffer < space) {
	    space = bytesInBuffer;
	}
	memcpy(result, RemovePoint(bufPtr), (size_t) space);
	bufPtr->nextRemoved += space;
	copied = space;

	end = result + copied;
	dst = result;
	for (src = result; src < end; src++) {
	    curByte = *src;
	    if (curByte == '\r') {
		SetFlag(statePtr, INPUT_SAW_CR);
		*dst = '\n';
		dst++;
	    } else {
		if ((curByte != '\n') || !(statePtr->flags & INPUT_SAW_CR)) {
		    *dst = (char) curByte;
		    dst++;
		}
		ResetFlag(statePtr, INPUT_SAW_CR);
	    }
	}
	copied = dst - result;
	break;
    }
    default:
	Tcl_Panic("unknown eol translation mode");
    }

    /*
     * If an in-stream EOF character is set for this channel, check that the
     * input we copied so far does not contain the EOF char. If it does, copy
     * only up to and excluding that character.
     */

    if (statePtr->inEofChar != 0) {
	for (i = 0; i < copied; i++) {
	    if (result[i] == (char) statePtr->inEofChar) {
		/*
		 * Set sticky EOF so that no further input is presented to the
		 * caller.
		 */

		SetFlag(statePtr, CHANNEL_EOF | CHANNEL_STICKY_EOF);
		statePtr->inputEncodingFlags |= TCL_ENCODING_END;
		copied = i;
		break;
	    }
	}
    }

    /*
     * If the current buffer is empty recycle it.
     */

    if (IsBufferEmpty(bufPtr)) {
	statePtr->inQueueHead = bufPtr->nextPtr;
	if (statePtr->inQueueHead == NULL) {
	    statePtr->inQueueTail = NULL;
	}
	RecycleBuffer(statePtr, bufPtr, 0);
    }

    /*
     * Return the number of characters copied into the result buffer. This may
     * be different from the number of bytes consumed, because of EOL
     * translations.
     */

    return copied;
}

/*
 *----------------------------------------------------------------------
 *
 * CopyBuffer --
 *
 *	Copy at most one buffer of input to the result space.
 *
 * Results:
 *	Number of bytes stored in the result buffer. May return zero if no
 *	input is available.
 *
 * Side effects:
 *	Consumes buffered input. May deallocate one buffer.
 *
 *----------------------------------------------------------------------
 */

static int
CopyBuffer(
    Channel *chanPtr,		/* Channel from which to read input. */
    char *result,		/* Where to store the copied input. */
    int space)			/* How many bytes are available in result to
				 * store the copied input? */
{
    ChannelBuffer *bufPtr;	/* The buffer from which to copy bytes. */
    int bytesInBuffer;		/* How many bytes are available to be copied
				 * in the current input buffer? */
    int copied;			/* How many characters were already copied
				 * into the destination space? */

    /*
     * If there is no input at all, return zero. The invariant is that either
     * there is no buffer in the queue, or if the first buffer is empty, it is
     * also the last buffer (and thus there is no input in the queue). Note
     * also that if the buffer is empty, we don't leave it in the queue, but
     * recycle it.
     */

    if (chanPtr->inQueueHead == NULL) {
	return 0;
    }
    bufPtr = chanPtr->inQueueHead;
    bytesInBuffer = BytesLeft(bufPtr);

    copied = 0;

    if (bytesInBuffer == 0) {
	RecycleBuffer(chanPtr->state, bufPtr, 0);
	chanPtr->inQueueHead = NULL;
	chanPtr->inQueueTail = NULL;
	return 0;
    }

    /*
     * Copy the current chunk into the result buffer.
     */

    if (bytesInBuffer < space) {
	space = bytesInBuffer;
    }

    memcpy(result, RemovePoint(bufPtr), (size_t) space);
    bufPtr->nextRemoved += space;
    copied = space;

    /*
     * We don't care about in-stream EOF characters here as the data read here
     * may still flow through one or more transformations, i.e. is not in its
     * final state yet.
     */

    /*
     * If the current buffer is empty recycle it.
     */

    if (IsBufferEmpty(bufPtr)) {
	chanPtr->inQueueHead = bufPtr->nextPtr;
	if (chanPtr->inQueueHead == NULL) {
	    chanPtr->inQueueTail = NULL;
	}
	RecycleBuffer(chanPtr->state, bufPtr, 0);
    }

    /*
     * Return the number of characters copied into the result buffer.
     */

    return copied;
}

/*
 *----------------------------------------------------------------------
 *
 * DoWrite --
 *
 *	Puts a sequence of characters into an output buffer, may queue the
 *	buffer for output if it gets full, and also remembers whether the
 *	current buffer is ready e.g. if it contains a newline and we are in
 *	line buffering mode.
 *
 * Results:
 *	The number of bytes written or -1 in case of error. If -1,
 *	Tcl_GetErrno will return the error code.
 *
 * Side effects:
 *	May buffer up output and may cause output to be produced on the
 *	channel.
 *
 *----------------------------------------------------------------------
 */

static int
DoWrite(
    Channel *chanPtr,		/* The channel to buffer output for. */
    const char *src,		/* Data to write. */
    int srcLen)			/* Number of bytes to write. */
{
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */
    ChannelBuffer *outBufPtr;	/* Current output buffer. */
    int foundNewline;		/* Did we find a newline in output? */
    char *dPtr;
    const char *sPtr;		/* Search variables for newline. */
    int crsent;			/* In CRLF eol translation mode, remember the
				 * fact that a CR was output to the channel
				 * without its following NL. */
    int i;			/* Loop index for newline search. */
    int destCopied;		/* How many bytes were used in this
				 * destination buffer to hold the output? */
    int totalDestCopied;	/* How many bytes total were copied to the
				 * channel buffer? */
    int srcCopied;		/* How many bytes were copied from the source
				 * string? */
    char *destPtr;		/* Where in line to copy to? */

    /*
     * If we are in network (or windows) translation mode, record the fact
     * that we have not yet sent a CR to the channel.
     */

    crsent = 0;

    /*
     * Loop filling buffers and flushing them until all output has been
     * consumed.
     */

    srcCopied = 0;
    totalDestCopied = 0;

    while (srcLen > 0) {
	/*
	 * Make sure there is a current output buffer to accept output.
	 */

	if (statePtr->curOutPtr == NULL) {
	    statePtr->curOutPtr = AllocChannelBuffer(statePtr->bufSize);
	}

	outBufPtr = statePtr->curOutPtr;

	destCopied = SpaceLeft(outBufPtr);
	if (destCopied > srcLen) {
	    destCopied = srcLen;
	}

	destPtr = InsertPoint(outBufPtr);
	switch (statePtr->outputTranslation) {
	case TCL_TRANSLATE_LF:
	    srcCopied = destCopied;
	    memcpy(destPtr, src, (size_t) destCopied);
	    break;
	case TCL_TRANSLATE_CR:
	    srcCopied = destCopied;
	    memcpy(destPtr, src, (size_t) destCopied);
	    for (dPtr = destPtr; dPtr < destPtr + destCopied; dPtr++) {
		if (*dPtr == '\n') {
		    *dPtr = '\r';
		}
	    }
	    break;
	case TCL_TRANSLATE_CRLF:
	    for (srcCopied = 0, dPtr = destPtr, sPtr = src;
		    dPtr < destPtr + destCopied;
		    dPtr++, sPtr++, srcCopied++) {
		if (*sPtr == '\n') {
		    if (crsent) {
			*dPtr = '\n';
			crsent = 0;
		    } else {
			*dPtr = '\r';
			crsent = 1;
			sPtr--, srcCopied--;
		    }
		} else {
		    *dPtr = *sPtr;
		}
	    }
	    break;
	case TCL_TRANSLATE_AUTO:
	    Tcl_Panic("Tcl_Write: AUTO output translation mode not supported");
	default:
	    Tcl_Panic("Tcl_Write: unknown output translation mode");
	}

	/*
	 * The current buffer is ready for output if it is full, or if it
	 * contains a newline and this channel is line-buffered, or if it
	 * contains any output and this channel is unbuffered.
	 */

	outBufPtr->nextAdded += destCopied;
	if (!(statePtr->flags & BUFFER_READY)) {
	    if (IsBufferFull(outBufPtr)) {
		SetFlag(statePtr, BUFFER_READY);
	    } else if (statePtr->flags & CHANNEL_LINEBUFFERED) {
		for (sPtr = src, i = 0, foundNewline = 0;
			(i < srcCopied) && (!foundNewline);
			i++, sPtr++) {
		    if (*sPtr == '\n') {
			foundNewline = 1;
			break;
		    }
		}
		if (foundNewline) {
		    SetFlag(statePtr, BUFFER_READY);
		}
	    } else if (statePtr->flags & CHANNEL_UNBUFFERED) {
		SetFlag(statePtr, BUFFER_READY);
	    }
	}

	totalDestCopied += srcCopied;
	src += srcCopied;
	srcLen -= srcCopied;

	if (statePtr->flags & BUFFER_READY) {
	    if (FlushChannel(NULL, chanPtr, 0) != 0) {
		return -1;
	    }
	}
    } /* Closes "while" */

    return totalDestCopied;
}

/*
 *----------------------------------------------------------------------
 *
 * CopyEventProc --
 *
 *	This routine is invoked as a channel event handler for the background
 *	copy operation. It is just a trivial wrapper around the CopyData
 *	routine.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
CopyEventProc(
    ClientData clientData,
    int mask)
{
    (void) CopyData((CopyState *) clientData, mask);
}

/*
 *----------------------------------------------------------------------
 *
 * StopCopy --
 *
 *	This routine halts a copy that is in progress.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Removes any pending channel handlers and restores the blocking and
 *	buffering modes of the channels. The CopyState is freed.
 *
 *----------------------------------------------------------------------
 */

static void
StopCopy(
    CopyState *csPtr)		/* State for bg copy to stop . */
{
    ChannelState *inStatePtr, *outStatePtr;
    int nonBlocking;

    if (!csPtr) {
	return;
    }

    inStatePtr = csPtr->readPtr->state;
    outStatePtr = csPtr->writePtr->state;

    /*
     * Restore the old blocking mode and output buffering mode.
     */

    nonBlocking = (csPtr->readFlags & CHANNEL_NONBLOCKING);
    if (nonBlocking != (inStatePtr->flags & CHANNEL_NONBLOCKING)) {
	SetBlockMode(NULL, csPtr->readPtr,
		nonBlocking ? TCL_MODE_NONBLOCKING : TCL_MODE_BLOCKING);
    }
    if (csPtr->readPtr != csPtr->writePtr) {
	nonBlocking = (csPtr->writeFlags & CHANNEL_NONBLOCKING);
	if (nonBlocking != (outStatePtr->flags & CHANNEL_NONBLOCKING)) {
	    SetBlockMode(NULL, csPtr->writePtr,
		    nonBlocking ? TCL_MODE_NONBLOCKING : TCL_MODE_BLOCKING);
	}
    }
    outStatePtr->flags &= ~(CHANNEL_LINEBUFFERED | CHANNEL_UNBUFFERED);
    outStatePtr->flags |=
	    csPtr->writeFlags & (CHANNEL_LINEBUFFERED | CHANNEL_UNBUFFERED);

    if (csPtr->cmdPtr) {
	Tcl_DeleteChannelHandler((Tcl_Channel) csPtr->readPtr, CopyEventProc,
		csPtr);
	if (csPtr->readPtr != csPtr->writePtr) {
	    Tcl_DeleteChannelHandler((Tcl_Channel) csPtr->writePtr,
		    CopyEventProc, csPtr);
	}
	TclDecrRefCount(csPtr->cmdPtr);
    }
    inStatePtr->csPtrR = NULL;
    outStatePtr->csPtrW = NULL;
    ckfree((char *) csPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * StackSetBlockMode --
 *
 *	This function sets the blocking mode for a channel, iterating through
 *	each channel in a stack and updates the state flags.
 *
 * Results:
 *	0 if OK, result code from failed blockModeProc otherwise.
 *
 * Side effects:
 *	Modifies the blocking mode of the channel and possibly generates an
 *	error.
 *
 *----------------------------------------------------------------------
 */

static int
StackSetBlockMode(
    Channel *chanPtr,		/* Channel to modify. */
    int mode)			/* One of TCL_MODE_BLOCKING or
				 * TCL_MODE_NONBLOCKING. */
{
    int result = 0;
    Tcl_DriverBlockModeProc *blockModeProc;

    /*
     * Start at the top of the channel stack
     */

    chanPtr = chanPtr->state->topChanPtr;
    while (chanPtr != NULL) {
	blockModeProc = Tcl_ChannelBlockModeProc(chanPtr->typePtr);
	if (blockModeProc != NULL) {
	    result = (*blockModeProc) (chanPtr->instanceData, mode);
	    if (result != 0) {
		Tcl_SetErrno(result);
		return result;
	    }
	}
	chanPtr = chanPtr->downChanPtr;
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * SetBlockMode --
 *
 *	This function sets the blocking mode for a channel and updates the
 *	state flags.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	Modifies the blocking mode of the channel and possibly generates an
 *	error.
 *
 *----------------------------------------------------------------------
 */

static int
SetBlockMode(
    Tcl_Interp *interp,		/* Interp for error reporting. */
    Channel *chanPtr,		/* Channel to modify. */
    int mode)			/* One of TCL_MODE_BLOCKING or
				 * TCL_MODE_NONBLOCKING. */
{
    int result = 0;
    ChannelState *statePtr = chanPtr->state;
				/* State info for channel */

    result = StackSetBlockMode(chanPtr, mode);
    if (result != 0) {
	if (interp != NULL) {
	    /*
	     * TIP #219.
	     * Move error messages put by the driver into the bypass area and
	     * put them into the regular interpreter result. Fall back to the
	     * regular message if nothing was found in the bypass.
	     *
	     * Note that we cannot have a message in the interpreter bypass
	     * area, StackSetBlockMode is restricted to the channel bypass.
	     * We still need the interp as the destination of the move.
	     */

	    if (!TclChanCaughtErrorBypass(interp, (Tcl_Channel) chanPtr)) {
		Tcl_AppendResult(interp, "error setting blocking mode: ",
			Tcl_PosixError(interp), NULL);
	    }
	} else {
	    /*
	     * TIP #219.
	     * If we have no interpreter to put a bypass message into we have
	     * to clear it, to prevent its propagation and use in other places
	     * unrelated to the actual occurence of the problem.
	     */

	    Tcl_SetChannelError((Tcl_Channel) chanPtr, NULL);
	}
	return TCL_ERROR;
    }
    if (mode == TCL_MODE_BLOCKING) {
	ResetFlag(statePtr, CHANNEL_NONBLOCKING | BG_FLUSH_SCHEDULED);
    } else {
	SetFlag(statePtr, CHANNEL_NONBLOCKING);
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetChannelNames --
 *
 *	Return the names of all open channels in the interp.
 *
 * Results:
 *	TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *	Interp result modified with list of channel names.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_GetChannelNames(
    Tcl_Interp *interp)		/* Interp for error reporting. */
{
    return Tcl_GetChannelNamesEx(interp, NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetChannelNamesEx --
 *
 *	Return the names of open channels in the interp filtered filtered
 *	through a pattern. If pattern is NULL, it returns all the open
 *	channels.
 *
 * Results:
 *	TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *	Interp result modified with list of channel names.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_GetChannelNamesEx(
    Tcl_Interp *interp,		/* Interp for error reporting. */
    const char *pattern)	/* Pattern to filter on. */
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    ChannelState *statePtr;
    const char *name;		/* Name for channel */
    Tcl_Obj *resultPtr;		/* Pointer to result object */
    Tcl_HashTable *hTblPtr;	/* Hash table of channels. */
    Tcl_HashEntry *hPtr;	/* Search variable. */
    Tcl_HashSearch hSearch;	/* Search variable. */

    if (interp == NULL) {
	return TCL_OK;
    }

    /*
     * Get the channel table that stores the channels registered for this
     * interpreter.
     */

    hTblPtr = GetChannelTable(interp);
    TclNewObj(resultPtr);
    if ((pattern != NULL) && TclMatchIsTrivial(pattern)
	    && !((pattern[0] == 's') && (pattern[1] == 't')
	    && (pattern[2] == 'd'))) {
	if ((Tcl_FindHashEntry(hTblPtr, pattern) != NULL)
		&& (Tcl_ListObjAppendElement(interp, resultPtr,
		Tcl_NewStringObj(pattern, -1)) != TCL_OK)) {
	    goto error;
	}
	goto done;
    }
    for (hPtr = Tcl_FirstHashEntry(hTblPtr, &hSearch); hPtr != NULL;
	    hPtr = Tcl_NextHashEntry(&hSearch)) {

	statePtr = ((Channel *) Tcl_GetHashValue(hPtr))->state;
	if (statePtr->topChanPtr == (Channel *) tsdPtr->stdinChannel) {
	    name = "stdin";
	} else if (statePtr->topChanPtr == (Channel *) tsdPtr->stdoutChannel) {
	    name = "stdout";
	} else if (statePtr->topChanPtr == (Channel *) tsdPtr->stderrChannel) {
	    name = "stderr";
	} else {
	    /*
	     * This is also stored in Tcl_GetHashKey(hTblPtr, hPtr), but it's
	     * simpler to just grab the name from the statePtr.
	     */

	    name = statePtr->channelName;
	}

	if (((pattern == NULL) || Tcl_StringMatch(name, pattern)) &&
		(Tcl_ListObjAppendElement(interp, resultPtr,
			Tcl_NewStringObj(name, -1)) != TCL_OK)) {
	error:
	    TclDecrRefCount(resultPtr);
	    return TCL_ERROR;
	}
    }

  done:
    Tcl_SetObjResult(interp, resultPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_IsChannelRegistered --
 *
 *	Checks whether the channel is associated with the interp. See also
 *	Tcl_RegisterChannel and Tcl_UnregisterChannel.
 *
 * Results:
 *	0 if the channel is not registered in the interpreter, 1 else.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_IsChannelRegistered(
    Tcl_Interp *interp,		/* The interp to query of the channel */
    Tcl_Channel chan)		/* The channel to check */
{
    Tcl_HashTable *hTblPtr;	/* Hash table of channels. */
    Tcl_HashEntry *hPtr;	/* Search variable. */
    Channel *chanPtr;		/* The real IO channel. */
    ChannelState *statePtr;	/* State of the real channel. */

    /*
     * Always check bottom-most channel in the stack. This is the one that
     * gets registered.
     */

    chanPtr = ((Channel *) chan)->state->bottomChanPtr;
    statePtr = chanPtr->state;

    hTblPtr = Tcl_GetAssocData(interp, "tclIO", NULL);
    if (hTblPtr == NULL) {
	return 0;
    }
    hPtr = Tcl_FindHashEntry(hTblPtr, statePtr->channelName);
    if (hPtr == NULL) {
	return 0;
    }
    if ((Channel *) Tcl_GetHashValue(hPtr) != chanPtr) {
	return 0;
    }

    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_IsChannelShared --
 *
 *	Checks whether the channel is shared by multiple interpreters.
 *
 * Results:
 *	A boolean value (0 = Not shared, 1 = Shared).
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_IsChannelShared(
    Tcl_Channel chan)		/* The channel to query */
{
    ChannelState *statePtr = ((Channel *) chan)->state;
				/* State of real channel structure. */

    return ((statePtr->refCount > 1) ? 1 : 0);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_IsChannelExisting --
 *
 *	Checks whether a channel of the given name exists in the
 *	(thread)-global list of all channels. See Tcl_GetChannelNamesEx for
 *	function exposed at the Tcl level.
 *
 * Results:
 *	A boolean value (0 = Does not exist, 1 = Does exist).
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_IsChannelExisting(
    const char *chanName)	/* The name of the channel to look for. */
{
    ChannelState *statePtr;
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    const char *name;
    int chanNameLen;

    chanNameLen = strlen(chanName);
    for (statePtr = tsdPtr->firstCSPtr; statePtr != NULL;
	    statePtr = statePtr->nextCSPtr) {
	if (statePtr->topChanPtr == (Channel *) tsdPtr->stdinChannel) {
	    name = "stdin";
	} else if (statePtr->topChanPtr == (Channel *) tsdPtr->stdoutChannel) {
	    name = "stdout";
	} else if (statePtr->topChanPtr == (Channel *) tsdPtr->stderrChannel) {
	    name = "stderr";
	} else {
	    name = statePtr->channelName;
	}

	if ((*chanName == *name) &&
		(memcmp(name, chanName, (size_t) chanNameLen) == 0)) {
	    return 1;
	}
    }

    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ChannelName --
 *
 *	Return the name of the channel type.
 *
 * Results:
 *	A pointer the name of the channel type.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

const char *
Tcl_ChannelName(
    const Tcl_ChannelType *chanTypePtr) /* Pointer to channel type. */
{
    return chanTypePtr->typeName;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ChannelVersion --
 *
 *	Return the of version of the channel type.
 *
 * Results:
 *	One of the TCL_CHANNEL_VERSION_* constants from tcl.h
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_ChannelTypeVersion
Tcl_ChannelVersion(
    const Tcl_ChannelType *chanTypePtr)
				/* Pointer to channel type. */
{
    if (chanTypePtr->version == TCL_CHANNEL_VERSION_2) {
	return TCL_CHANNEL_VERSION_2;
    } else if (chanTypePtr->version == TCL_CHANNEL_VERSION_3) {
	return TCL_CHANNEL_VERSION_3;
    } else if (chanTypePtr->version == TCL_CHANNEL_VERSION_4) {
	return TCL_CHANNEL_VERSION_4;
    } else if (chanTypePtr->version == TCL_CHANNEL_VERSION_5) {
	return TCL_CHANNEL_VERSION_5;
    } else {
	/*
	 * In <v2 channel versions, the version field is occupied by the
	 * Tcl_DriverBlockModeProc
	 */

	return TCL_CHANNEL_VERSION_1;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * HaveVersion --
 *
 *	Return whether a channel type is (at least) of a given version.
 *
 * Results:
 *	True if the minimum version is exceeded by the version actually
 *	present.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
HaveVersion(
    const Tcl_ChannelType *chanTypePtr,
    Tcl_ChannelTypeVersion minimumVersion)
{
    Tcl_ChannelTypeVersion actualVersion = Tcl_ChannelVersion(chanTypePtr);

    return (PTR2INT(actualVersion)) >= (PTR2INT(minimumVersion));
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ChannelBlockModeProc --
 *
 *	Return the Tcl_DriverBlockModeProc of the channel type.
 *
 * Results:
 *	A pointer to the proc.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------- */

Tcl_DriverBlockModeProc *
Tcl_ChannelBlockModeProc(
    const Tcl_ChannelType *chanTypePtr)
				/* Pointer to channel type. */
{
    if (HaveVersion(chanTypePtr, TCL_CHANNEL_VERSION_2)) {
	return chanTypePtr->blockModeProc;
    } else {
	/*
	 * The v1 structure had the blockModeProc in a different place.
	 */

	return (Tcl_DriverBlockModeProc *) (chanTypePtr->version);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ChannelCloseProc --
 *
 *	Return the Tcl_DriverCloseProc of the channel type.
 *
 * Results:
 *	A pointer to the proc.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_DriverCloseProc *
Tcl_ChannelCloseProc(
    const Tcl_ChannelType *chanTypePtr)
				/* Pointer to channel type. */
{
    return chanTypePtr->closeProc;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ChannelClose2Proc --
 *
 *	Return the Tcl_DriverClose2Proc of the channel type.
 *
 * Results:
 *	A pointer to the proc.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_DriverClose2Proc *
Tcl_ChannelClose2Proc(
    const Tcl_ChannelType *chanTypePtr)
				/* Pointer to channel type. */
{
    return chanTypePtr->close2Proc;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ChannelInputProc --
 *
 *	Return the Tcl_DriverInputProc of the channel type.
 *
 * Results:
 *	A pointer to the proc.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_DriverInputProc *
Tcl_ChannelInputProc(
    const Tcl_ChannelType *chanTypePtr)
				/* Pointer to channel type. */
{
    return chanTypePtr->inputProc;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ChannelOutputProc --
 *
 *	Return the Tcl_DriverOutputProc of the channel type.
 *
 * Results:
 *	A pointer to the proc.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_DriverOutputProc *
Tcl_ChannelOutputProc(
    const Tcl_ChannelType *chanTypePtr)
				/* Pointer to channel type. */
{
    return chanTypePtr->outputProc;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ChannelSeekProc --
 *
 *	Return the Tcl_DriverSeekProc of the channel type.
 *
 * Results:
 *	A pointer to the proc.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_DriverSeekProc *
Tcl_ChannelSeekProc(
    const Tcl_ChannelType *chanTypePtr)
				/* Pointer to channel type. */
{
    return chanTypePtr->seekProc;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ChannelSetOptionProc --
 *
 *	Return the Tcl_DriverSetOptionProc of the channel type.
 *
 * Results:
 *	A pointer to the proc.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_DriverSetOptionProc *
Tcl_ChannelSetOptionProc(
    const Tcl_ChannelType *chanTypePtr)
				/* Pointer to channel type. */
{
    return chanTypePtr->setOptionProc;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ChannelGetOptionProc --
 *
 *	Return the Tcl_DriverGetOptionProc of the channel type.
 *
 * Results:
 *	A pointer to the proc.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_DriverGetOptionProc *
Tcl_ChannelGetOptionProc(
    const Tcl_ChannelType *chanTypePtr)
				/* Pointer to channel type. */
{
    return chanTypePtr->getOptionProc;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ChannelWatchProc --
 *
 *	Return the Tcl_DriverWatchProc of the channel type.
 *
 * Results:
 *	A pointer to the proc.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_DriverWatchProc *
Tcl_ChannelWatchProc(
    const Tcl_ChannelType *chanTypePtr)
				/* Pointer to channel type. */
{
    return chanTypePtr->watchProc;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ChannelGetHandleProc --
 *
 *	Return the Tcl_DriverGetHandleProc of the channel type.
 *
 * Results:
 *	A pointer to the proc.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_DriverGetHandleProc *
Tcl_ChannelGetHandleProc(
    const Tcl_ChannelType *chanTypePtr)
				/* Pointer to channel type. */
{
    return chanTypePtr->getHandleProc;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ChannelFlushProc --
 *
 *	Return the Tcl_DriverFlushProc of the channel type.
 *
 * Results:
 *	A pointer to the proc.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_DriverFlushProc *
Tcl_ChannelFlushProc(
    const Tcl_ChannelType *chanTypePtr)
				/* Pointer to channel type. */
{
    if (HaveVersion(chanTypePtr, TCL_CHANNEL_VERSION_2)) {
	return chanTypePtr->flushProc;
    } else {
	return NULL;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ChannelHandlerProc --
 *
 *	Return the Tcl_DriverHandlerProc of the channel type.
 *
 * Results:
 *	A pointer to the proc.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_DriverHandlerProc *
Tcl_ChannelHandlerProc(
    const Tcl_ChannelType *chanTypePtr)
				/* Pointer to channel type. */
{
    if (HaveVersion(chanTypePtr, TCL_CHANNEL_VERSION_2)) {
	return chanTypePtr->handlerProc;
    } else {
	return NULL;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ChannelWideSeekProc --
 *
 *	Return the Tcl_DriverWideSeekProc of the channel type.
 *
 * Results:
 *	A pointer to the proc.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_DriverWideSeekProc *
Tcl_ChannelWideSeekProc(
    const Tcl_ChannelType *chanTypePtr)
				/* Pointer to channel type. */
{
    if (HaveVersion(chanTypePtr, TCL_CHANNEL_VERSION_3)) {
	return chanTypePtr->wideSeekProc;
    } else {
	return NULL;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ChannelThreadActionProc --
 *
 *	TIP #218, Channel Thread Actions. Return the
 *	Tcl_DriverThreadActionProc of the channel type.
 *
 * Results:
 *	A pointer to the proc.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_DriverThreadActionProc *
Tcl_ChannelThreadActionProc(
    const Tcl_ChannelType *chanTypePtr)
				/* Pointer to channel type. */
{
    if (HaveVersion(chanTypePtr, TCL_CHANNEL_VERSION_4)) {
	return chanTypePtr->threadActionProc;
    } else {
	return NULL;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetChannelErrorInterp --
 *
 *	TIP #219, Tcl Channel Reflection API.
 *	Store an error message for the I/O system.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Discards a previously stored message.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_SetChannelErrorInterp(
    Tcl_Interp *interp,		/* Interp to store the data into. */
    Tcl_Obj *msg)		/* Error message to store. */
{
    Interp *iPtr = (Interp *) interp;

    if (iPtr->chanMsg != NULL) {
	TclDecrRefCount(iPtr->chanMsg);
	iPtr->chanMsg = NULL;
    }

    if (msg != NULL) {
	iPtr->chanMsg = FixLevelCode(msg);
	Tcl_IncrRefCount(iPtr->chanMsg);
    }
    return;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetChannelError --
 *
 *	TIP #219, Tcl Channel Reflection API.
 *	Store an error message for the I/O system.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Discards a previously stored message.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_SetChannelError(
    Tcl_Channel chan,		/* Channel to store the data into. */
    Tcl_Obj *msg)		/* Error message to store. */
{
    ChannelState *statePtr = ((Channel *) chan)->state;

    if (statePtr->chanMsg != NULL) {
	TclDecrRefCount(statePtr->chanMsg);
	statePtr->chanMsg = NULL;
    }

    if (msg != NULL) {
	statePtr->chanMsg = FixLevelCode(msg);
	Tcl_IncrRefCount(statePtr->chanMsg);
    }
    return;
}

/*
 *----------------------------------------------------------------------
 *
 * FixLevelCode --
 *
 *	TIP #219, Tcl Channel Reflection API.
 *	Scans an error message for bad -code / -level directives. Returns a
 *	modified copy with such directives corrected, and the input if it had
 *	no problems.
 *
 * Results:
 *	A Tcl_Obj*
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj *
FixLevelCode(
    Tcl_Obj *msg)
{
    int explicitResult, numOptions, lc, lcn;
    Tcl_Obj **lv, **lvn;
    int res, i, j, val, lignore, cignore;
    int newlevel = -1, newcode = -1;

    /* ASSERT msg != NULL */

    /*
     * Process the caught message.
     *
     * Syntax = (option value)... ?message?
     *
     * Bad message syntax causes a panic, because the other side uses
     * Tcl_GetReturnOptions and list construction functions to marshall the
     * information. Hence an error means that we've got serious breakage.
     */

    res = Tcl_ListObjGetElements(NULL, msg, &lc, &lv);
    if (res != TCL_OK) {
	Tcl_Panic("Tcl_SetChannelError(Interp): Bad syntax of message");
    }

    explicitResult = (1 == (lc % 2));
    numOptions = lc - explicitResult;

    /*
     * No options, nothing to do.
     */

    if (numOptions == 0) {
	return msg;
    }

    /*
     * Check for -code x, x != 1|error, and -level x, x != 0
     */

    for (i = 0; i < numOptions; i += 2) {
	if (0 == strcmp(TclGetString(lv[i]), "-code")) {
	    /*
	     * !"error", !integer, integer != 1 (numeric code for error)
	     */

	    res = TclGetIntFromObj(NULL, lv[i+1], &val);
	    if (((res == TCL_OK) && (val != 1)) || ((res != TCL_OK) &&
		    (0 != strcmp(TclGetString(lv[i+1]), "error")))) {
		newcode = 1;
	    }
	} else if (0 == strcmp(TclGetString(lv[i]), "-level")) {
	    /*
	     * !integer, integer != 0
	     */

	    res = TclGetIntFromObj(NULL, lv [i+1], &val);
	    if ((res != TCL_OK) || (val != 0)) {
		newlevel = 0;
	    }
	}
    }

    /*
     * -code, -level are either not present or ok. Nothing to do.
     */

    if ((newlevel < 0) && (newcode < 0)) {
	return msg;
    }

    lcn = numOptions;
    if (explicitResult) {
	lcn ++;
    }
    if (newlevel >= 0) {
	lcn += 2;
    }
    if (newcode >= 0) {
	lcn += 2;
    }

    lvn = (Tcl_Obj **) ckalloc(lcn * sizeof(Tcl_Obj *));

    /*
     * New level/code information is spliced into the first occurence of
     * -level, -code, further occurences are ignored. The options cannot be
     * not present, we would not come here. Options which are ok are simply
     * copied over.
     */

    lignore = cignore = 0;
    for (i=0, j=0; i<numOptions; i+=2) {
	if (0 == strcmp(TclGetString(lv[i]), "-level")) {
	    if (newlevel >= 0) {
		lvn[j++] = lv[i];
		lvn[j++] = Tcl_NewIntObj(newlevel);
		newlevel = -1;
		lignore = 1;
		continue;
	    } else if (lignore) {
		continue;
	    }
	} else if (0 == strcmp(TclGetString(lv[i]), "-code")) {
	    if (newcode >= 0) {
		lvn[j++] = lv[i];
		lvn[j++] = Tcl_NewIntObj(newcode);
		newcode = -1;
		cignore = 1;
		continue;
	    } else if (cignore) {
		continue;
	    }
	}

	/*
	 * Keep everything else, possibly copied down.
	 */

	lvn[j++] = lv[i];
	lvn[j++] = lv[i+1];
    }
    if (newlevel >= 0) {
	Tcl_Panic("Defined newlevel not used in rewrite");
    }
    if (newcode >= 0) {
	Tcl_Panic("Defined newcode not used in rewrite");
    }

    if (explicitResult) {
	lvn[j++] = lv[i];
    }

    msg = Tcl_NewListObj(j, lvn);

    ckfree((char *) lvn);
    return msg;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetChannelErrorInterp --
 *
 *	TIP #219, Tcl Channel Reflection API.
 *	Return the message stored by the channel driver.
 *
 * Results:
 *	Tcl error message object.
 *
 * Side effects:
 *	Resets the stored data to NULL.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_GetChannelErrorInterp(
    Tcl_Interp *interp,		/* Interp to query. */
    Tcl_Obj **msg)		/* Place for error message. */
{
    Interp *iPtr = (Interp *) interp;

    *msg = iPtr->chanMsg;
    iPtr->chanMsg = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetChannelError --
 *
 *	TIP #219, Tcl Channel Reflection API.
 *	Return the message stored by the channel driver.
 *
 * Results:
 *	Tcl error message object.
 *
 * Side effects:
 *	Resets the stored data to NULL.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_GetChannelError(
    Tcl_Channel chan,		/* Channel to query. */
    Tcl_Obj **msg)		/* Place for error message. */
{
    ChannelState *statePtr = ((Channel *) chan)->state;

    *msg = statePtr->chanMsg;
    statePtr->chanMsg = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ChannelTruncateProc --
 *
 *	TIP #208 (subsection relating to truncation, based on TIP #206).
 *	Return the Tcl_DriverTruncateProc of the channel type.
 *
 * Results:
 *	A pointer to the proc.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_DriverTruncateProc *
Tcl_ChannelTruncateProc(
    const Tcl_ChannelType *chanTypePtr)
				/* Pointer to channel type. */
{
    if (HaveVersion(chanTypePtr, TCL_CHANNEL_VERSION_5)) {
	return chanTypePtr->truncateProc;
    } else {
	return NULL;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * DupChannelIntRep --
 *
 *	Initialize the internal representation of a new Tcl_Obj to a copy of
 *	the internal representation of an existing string object.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	copyPtr's internal rep is set to a copy of srcPtr's internal
 *	representation.
 *
 *----------------------------------------------------------------------
 */

static void
DupChannelIntRep(
    register Tcl_Obj *srcPtr,	/* Object with internal rep to copy. Must have
				 * an internal rep of type "Channel". */
    register Tcl_Obj *copyPtr)	/* Object with internal rep to set. Must not
				 * currently have an internal rep.*/
{
    ChannelState *statePtr = GET_CHANNELSTATE(srcPtr);
    SET_CHANNELSTATE(copyPtr, statePtr);
    Tcl_Preserve((ClientData) statePtr);
    copyPtr->typePtr = &tclChannelType;
}

/*
 *----------------------------------------------------------------------
 *
 * SetChannelFromAny --
 *
 *	Create an internal representation of type "Channel" for an object.
 *
 * Results:
 *	This operation always succeeds and returns TCL_OK.
 *
 * Side effects:
 *	Any old internal reputation for objPtr is freed and the internal
 *	representation is set to "Channel".
 *
 *----------------------------------------------------------------------
 */

static int
SetChannelFromAny(
    Tcl_Interp *interp,		/* Used for error reporting if not NULL. */
    register Tcl_Obj *objPtr)	/* The object to convert. */
{
    ChannelState *statePtr;

    if (objPtr->typePtr == &tclChannelType) {
	/*
	 * The channel is valid until any call to DetachChannel occurs.
	 * Ensure consistency checks are done.
	 */
	statePtr = GET_CHANNELSTATE(objPtr);
	if (statePtr->flags & (CHANNEL_TAINTED|CHANNEL_CLOSED)) {
	    ResetFlag(statePtr, CHANNEL_TAINTED);
	    Tcl_Release((ClientData) statePtr);
	    UpdateStringOfChannel(objPtr);
	    objPtr->typePtr = NULL;
	}
    }
    if (objPtr->typePtr != &tclChannelType) {
	Tcl_Channel chan;

	/*
	 * We need a valid string with which to check for a valid channel, but
	 * make sure not to free internal rep until validated. [Bug 1847044]
	 */
	if ((objPtr->typePtr != NULL) && (objPtr->bytes == NULL)) {
	    objPtr->typePtr->updateStringProc(objPtr);
	}

	chan = Tcl_GetChannel(interp, objPtr->bytes, NULL);
	if (chan == NULL) {
	    return TCL_ERROR;
	}

	TclFreeIntRep(objPtr);
	statePtr = ((Channel *)chan)->state;
	Tcl_Preserve((ClientData) statePtr);
	SET_CHANNELSTATE(objPtr, statePtr);
	objPtr->typePtr = &tclChannelType;
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UpdateStringOfChannel --
 *
 *	Update the string representation for an object whose internal
 *	representation is "Channel".
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The object's string may be set by converting its Unicode represention
 *	to UTF format.
 *
 *----------------------------------------------------------------------
 */

static void
UpdateStringOfChannel(
    Tcl_Obj *objPtr)		/* Object with string rep to update. */
{
    if (objPtr->bytes == NULL) {
	ChannelState *statePtr = GET_CHANNELSTATE(objPtr);
	const char *name = statePtr->channelName;
	if (name) {
	    size_t len = strlen(name);
	    objPtr->bytes = (char *) ckalloc(len + 1);
	    objPtr->length = len;
	    memcpy(objPtr->bytes, name, len);
	} else {
	    objPtr->bytes = tclEmptyStringRep;
	    objPtr->length = 0;
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * FreeChannelIntRep --
 *
 *	Release statePtr storage.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	May cause state to be freed.
 *
 *----------------------------------------------------------------------
 */

static void
FreeChannelIntRep(
    Tcl_Obj *objPtr)		/* Object with internal rep to free. */
{
    Tcl_Release((ClientData) GET_CHANNELSTATE(objPtr));
}

#if 0
/*
 * For future debugging work, a simple function to print the flags of a
 * channel in semi-readable form.
 */

static int
DumpFlags(
    char *str,
    int flags)
{
    char buf[20];
    int i = 0;

#define ChanFlag(chr,bit) (buf[i++] = ((flags & (bit)) ? (chr) : '_'))

    ChanFlag('r', TCL_READABLE);
    ChanFlag('w', TCL_WRITABLE);
    ChanFlag('n', CHANNEL_NONBLOCKING);
    ChanFlag('l', CHANNEL_LINEBUFFERED);
    ChanFlag('u', CHANNEL_UNBUFFERED);
    ChanFlag('R', BUFFER_READY);
    ChanFlag('F', BG_FLUSH_SCHEDULED);
    ChanFlag('c', CHANNEL_CLOSED);
    ChanFlag('E', CHANNEL_EOF);
    ChanFlag('S', CHANNEL_STICKY_EOF);
    ChanFlag('B', CHANNEL_BLOCKED);
    ChanFlag('/', INPUT_SAW_CR);
    ChanFlag('*', INPUT_NEED_NL);
    ChanFlag('D', CHANNEL_DEAD);
    ChanFlag('R', CHANNEL_RAW_MODE);
#ifdef TCL_IO_TRACK_OS_FOR_DRIVER_WITH_BAD_BLOCKING
    ChanFlag('T', CHANNEL_TIMER_FEV);
    ChanFlag('H', CHANNEL_HAS_MORE_DATA);
#endif /* TCL_IO_TRACK_OS_FOR_DRIVER_WITH_BAD_BLOCKING */
    ChanFlag('x', CHANNEL_INCLOSE);

    buf[i] ='\0';

    fprintf(stderr, "%s: %s\n", str, buf);
    return 0;
}
#endif

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
