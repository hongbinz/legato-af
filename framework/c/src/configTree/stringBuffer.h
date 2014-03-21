
// -------------------------------------------------------------------------------------------------
/*
 *  Helper code for maintaining largish buffers of string memory.
 *
 *  Copyright (C) Sierra Wireless, Inc. 2013. All rights reserved. Use of this work is subject to license.
 */
// -------------------------------------------------------------------------------------------------

#ifndef CFG_STRING_BUFFER_INCLUDE_GUARD
#define CFG_STRING_BUFFER_INCLUDE_GUARD




#define SB_SIZE 512




//--------------------------------------------------------------------------------------------------
/**
 *  Init the buffer pool this code depends on.
 */
//--------------------------------------------------------------------------------------------------
void sb_Init
(
);




//--------------------------------------------------------------------------------------------------
/**
 *  Allocate a new string from our pool.
 */
//--------------------------------------------------------------------------------------------------
char* sb_Get
(
);




//--------------------------------------------------------------------------------------------------
/**
 *  Crate a new copy of an existing string buffer object.
 */
//--------------------------------------------------------------------------------------------------
char* sb_NewCopy
(
    const char* stringPtr  ///< The buffer to duplicate.
);




//--------------------------------------------------------------------------------------------------
/**
 *  Release back the string into the pool.
 */
//--------------------------------------------------------------------------------------------------
void sb_Release
(
    char* bufferPtr  ///< The string to release.
);




#endif