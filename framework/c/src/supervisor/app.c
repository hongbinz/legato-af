//--------------------------------------------------------------------------------------------------
/** @file supervisor/app.c
 *
 * This is the Supervisor application class.
 *
 * To instantiate an application object of this class use the app_Create() API.
 *
 * When an app object is created it sets up the working area for the application.  The working area
 * for an application is under the directory CURRENT_SYSTEM_PATH/appsWriteable/<appName>.
 *
 * Links to files that are needed by the app are created in the working area.  Bind mounts are used
 * to create links for sandboxed apps.  Symlinks are used to create links for unsandboxed apps.
 *
 * For sandboxed apps links are created to default libs/files, required dirs/files and readonly
 * bundled files.  A tmpfs is also mounted under CURRENT_SYSTEM_PATH/appsWriteable/<appName>/tmp.
 * This working area is the sandbox for the app.
 *
 * For unsandboxed apps links are created to required dirs/files and readonly bundled files.
 *
 * The reason that only readonly bundled files are linked into the working area is because the
 * writable bundled files are already copied into the working area by the app installer.
 *
 * Generally, only links to files are created because links to directories can lead to unexpected
 * behaviours.  For instance, if a link to a directory is created and then a link to a file under
 * that directory is created.  The linked file will not show up in the linked directory both inside
 * the app's working area and the in the directory's original location.
 * So, instead when a directory is required or bundled, all files in the directory are individually
 * linked.
 *
 * The working area is not cleaned up by the Supervisor, rather it is left to the installer to
 * clean up.
 *
 * @todo Implement support for dynamic files.
 *
 * The application objects instantiated by this class contains a list of process object containers
 * that belong to the application.  This list of processes is used to manage all processes that
 * need to be started for the application.  However, an application may contain other processes not
 * in this list, processes that were forked by processes within the app.
 *
 * Copyright (C) Sierra Wireless Inc. Use of this work is subject to license.
 */

#include "legato.h"
#include "watchdogAction.h"
#include "app.h"
#include "limit.h"
#include "proc.h"
#include "user.h"
#include "le_cfg_interface.h"
#include "resourceLimits.h"
#include "smack.h"
#include "cgroups.h"
#include "killProc.h"
#include "interfaces.h"
#include "sysPaths.h"
#include "devSmack.h"
#include "dir.h"
#include "fileDescriptor.h"
#include "fileSystem.h"


//--------------------------------------------------------------------------------------------------
/**
 * The name of the node in the config tree that specifies whether the app should be in a sandbox.
 *
 * If this entry in the config tree is missing or empty the application will be sandboxed.
 */
//--------------------------------------------------------------------------------------------------
#define CFG_NODE_SANDBOXED                              "sandboxed"


//--------------------------------------------------------------------------------------------------
/**
 * The name of the node in the config tree that contains a process's supplementary groups list.
 *
 * Supplementary groups list is only available for non-sandboxed apps.
 */
//--------------------------------------------------------------------------------------------------
#define CFG_NODE_GROUPS                                 "groups"


//--------------------------------------------------------------------------------------------------
/**
 * The name of the node in the config tree that contains the list of processes for the application.
 *
 * If this entry in the config tree is missing or empty the application will not be launched.
 */
//--------------------------------------------------------------------------------------------------
#define CFG_NODE_PROC_LIST                              "procs"


//--------------------------------------------------------------------------------------------------
/**
 * The name of the node in the config tree that contains the list of bindings for the application.
 */
//--------------------------------------------------------------------------------------------------
#define CFG_NODE_BINDINGS                               "bindings"


//--------------------------------------------------------------------------------------------------
/**
 * The name of the node in the config tree that contains the list of required files and directories.
 */
//--------------------------------------------------------------------------------------------------
#define CFG_NODE_REQUIRES                               "requires"


//--------------------------------------------------------------------------------------------------
/**
 * The name of the node in the config tree that contains the list of import directives for
 * devices that an application needs.
 */
//--------------------------------------------------------------------------------------------------
#define CFG_NODE_DEVICES                                "devices"


//--------------------------------------------------------------------------------------------------
/**
 * The name of the node in the config tree that contains the list of bundled files and directories.
 */
//--------------------------------------------------------------------------------------------------
#define CFG_NODE_BUNDLES                                "bundles"


//--------------------------------------------------------------------------------------------------
/**
 * The name of the node in the config tree that contains the list of required files and directories.
 */
//--------------------------------------------------------------------------------------------------
#define CFG_NODE_REQUIRES                               "requires"


//--------------------------------------------------------------------------------------------------
/**
 * The name of the node in the config tree that contains the list of import directives for files
 * that an application needs.
 */
//--------------------------------------------------------------------------------------------------
#define CFG_NODE_FILES                                  "files"


//--------------------------------------------------------------------------------------------------
/**
 * The name of the node in the config tree that contains the list of import directives for
 * directories that an application needs.
 */
//--------------------------------------------------------------------------------------------------
#define CFG_NODE_DIRS                                   "dirs"


//--------------------------------------------------------------------------------------------------
/**
 * The name of the node in the config tree that contains the list of import directives for
 * devices that an application needs.
 */
//--------------------------------------------------------------------------------------------------
#define CFG_NODE_DEVICES                                "devices"


//--------------------------------------------------------------------------------------------------
/**
 * Maximum number of bytes in a permission string for devices.
 */
//--------------------------------------------------------------------------------------------------
#define MAX_DEVICE_PERM_STR_BYTES                       3


//--------------------------------------------------------------------------------------------------
/**
 * File link object.  Used to hold links that should be created for applications.
 */
//--------------------------------------------------------------------------------------------------
typedef struct
{
    char src[LIMIT_MAX_PATH_BYTES];     ///< Absolute path to the source file.
    char dest[LIMIT_MAX_PATH_BYTES];    ///< Dest path relative to the application's runtime area.
                                        ///  If this ends in a separator then it is a directory else
                                        ///  it is a file.
}
FileLinkObj_t;


#if ! defined LE_SVCDIR_SERVER_SOCKET_NAME
    #error LE_SVCDIR_SERVER_SOCKET_NAME not defined.
#endif
#if ! defined LE_SVCDIR_CLIENT_SOCKET_NAME
    #error LE_SVCDIR_CLIENT_SOCKET_NAME not defined.
#endif
//--------------------------------------------------------------------------------------------------
/**
 * Files to link into all sandboxed applications by default.
 */
//--------------------------------------------------------------------------------------------------
static const FileLinkObj_t DefaultLinks[] =
{
    {.src = "/dev/log", .dest = "/dev/"},
    {.src = "/dev/null", .dest = "/dev/"},
    {.src = "/dev/zero", .dest = "/dev/"},
    {.src = CURRENT_SYSTEM_PATH"/lib/liblegato.so", .dest = "/lib/"}
};


//--------------------------------------------------------------------------------------------------
/**
 * Files to link into all sandboxed applications' /tmp directory by default.
 */
//--------------------------------------------------------------------------------------------------
static const FileLinkObj_t DefaultTmpLinks[] =
{
    {.src = STRINGIZE(LE_SVCDIR_SERVER_SOCKET_NAME), .dest = "/tmp/legato/"},
    {.src = STRINGIZE(LE_SVCDIR_CLIENT_SOCKET_NAME), .dest = "/tmp/legato/"}
};


#if defined(TARGET_IMPORTS_X86_64)

//--------------------------------------------------------------------------------------------------
/**
 * Files and directories to link into all applications by default for the default system.
 */
//--------------------------------------------------------------------------------------------------
static const FileLinkObj_t DefaultSystemLinks[] =
{
    {.src = "/lib/ld-linux-x86-64.so.2", .dest = "/lib/"},
    {.src = "/lib/libc.so.6", .dest = "/lib/"},
    {.src = "/lib/libpthread.so.0", .dest = "/lib/"},
    {.src = "/lib/librt.so.1", .dest = "/lib/"},
    {.src = "/lib/libdl.so.2", .dest = "/lib/"},
    {.src = "/lib/libgcc_s.so.1", .dest = "/lib/"},
    {.src = "/lib/libm.so.6", .dest = "/lib/"},
    {.src = "/usr/lib/libstdc++.so.6", .dest = "/lib/"}
};

#elif defined(TARGET_IMPORTS_X86)

//--------------------------------------------------------------------------------------------------
/**
 * Files and directories to import into all applications by default for the default system.
 */
//--------------------------------------------------------------------------------------------------
static const FileLinkObj_t DefaultSystemLinks[] =
{
    {.src = "/lib/ld-linux.so.2", .dest = "/lib/"},
    {.src = "/lib/libc.so.6", .dest = "/lib/"},
    {.src = "/lib/libpthread.so.0", .dest = "/lib/"},
    {.src = "/lib/librt.so.1", .dest = "/lib/"},
    {.src = "/lib/libdl.so.2", .dest = "/lib/"},
    {.src = "/lib/libgcc_s.so.1", .dest = "/lib/"},
    {.src = "/lib/libm.so.6", .dest = "/lib/"},
    {.src = "/usr/lib/libstdc++.so.6", .dest = "/lib/"}
};

#elif defined(TARGET_IMPORTS_ARMV6) || defined(TARGET_IMPORTS_ARMV7)

//--------------------------------------------------------------------------------------------------
/**
 * Files and directories to import into all applications by default for the default system.
 */
//--------------------------------------------------------------------------------------------------
static const FileLinkObj_t DefaultSystemLinks[] =
{
    {.src = "/lib/ld-linux.so.3", .dest = "/lib/"},
    {.src = "/lib/libc.so.6", .dest = "/lib/"},
    {.src = "/lib/libpthread.so.0", .dest = "/lib/"},
    {.src = "/lib/librt.so.1", .dest = "/lib/"},
    {.src = "/lib/libdl.so.2", .dest = "/lib/"},
    {.src = "/lib/libgcc_s.so.1", .dest = "/lib/"},
    {.src = "/lib/libm.so.6", .dest = "/lib/"},
    {.src = "/usr/lib/libstdc++.so.6", .dest = "/lib/"}
};

#elif defined(TARGET_IMPORTS_RASPI)

//--------------------------------------------------------------------------------------------------
/**
 * Files and directories to import into all applications by default for the default system.
 */
//--------------------------------------------------------------------------------------------------
static const FileLinkObj_t DefaultSystemLinks[] =
{
    {.src = "/lib/ld-linux.so.3", .dest = "/lib/"},
    {.src = "/lib/ld-linux-armhf.so.3", .dest = "/lib/"},
    {.src = "/lib/arm-linux-gnueabihf/libc.so.6", .dest = "/lib/"},
    {.src = "/lib/arm-linux-gnueabihf/libpthread.so.0", .dest = "/lib/"},
    {.src = "/lib/arm-linux-gnueabihf/librt.so.1", .dest = "/lib/"},
    {.src = "/lib/arm-linux-gnueabihf/libdl.so.2", .dest = "/lib/"},
    {.src = "/lib/arm-linux-gnueabihf/libgcc_s.so.1", .dest = "/lib/"},
    {.src = "/lib/arm-linux-gnueabihf/libm.so.6", .dest = "/lib/"},
    {.src = "/usr/lib/arm-linux-gnueabihf/libstdc++.so.6", .dest = "/lib/"}
};

#else
#error No "TARGET_IMPORTS_x" defined.
#endif


//--------------------------------------------------------------------------------------------------
/**
 * Timeout value for killing processes in an app.
 */
//--------------------------------------------------------------------------------------------------
static const le_clk_Time_t KillTimeout =
{
        .sec = 1,
        .usec = 0
};


//--------------------------------------------------------------------------------------------------
/**
 * The application object.
 */
//--------------------------------------------------------------------------------------------------
typedef struct app_Ref
{
    char*           name;                               // Name of the application.
    char            cfgPathRoot[LIMIT_MAX_PATH_BYTES];  // Our path in the config tree.
    bool            sandboxed;                          // true if this is a sandboxed app.
    char            installDirPath[LIMIT_MAX_PATH_BYTES]; // Abs path to install files dir.
    char            workingDir[LIMIT_MAX_PATH_BYTES];   // Abs path to the apps working directory.
    uid_t           uid;                // User ID for this application.
    gid_t           gid;                // Group ID for this application.
    gid_t           supplementGids[LIMIT_MAX_NUM_SUPPLEMENTARY_GROUPS];  // List of supplementary
                                                                         // group IDs.
    size_t          numSupplementGids;  // Number of supplementary groups for this app.
    app_State_t     state;              // Applications current state.
    le_dls_List_t   procs;              // List of processes in this application.
    le_dls_List_t   auxProcs;           // List of auxiliary processes in this application.
    le_timer_Ref_t  killTimer;          // Timeout timer for killing processes.
}
App_t;


//--------------------------------------------------------------------------------------------------
/**
 * The memory pool for application objects.
 */
//--------------------------------------------------------------------------------------------------
static le_mem_PoolRef_t AppPool;


//--------------------------------------------------------------------------------------------------
/**
 * Prototype for process stopped handler.
 */
//--------------------------------------------------------------------------------------------------
typedef le_result_t (*ProcStopHandler_t)
(
    proc_Ref_t procRef                  ///< [IN] The stopped process
);


//--------------------------------------------------------------------------------------------------
/**
 * The process container object.
 */
//--------------------------------------------------------------------------------------------------
typedef struct app_ProcRef
{
    proc_Ref_t      procRef;        // The process reference.
    ProcStopHandler_t stopHandler;  // Handler function that gets called when this process stops.
    le_dls_Link_t   link;           // The link in the application's list of processes.
    app_Proc_StopHandlerFunc_t externStopHandler;   // External stop handler.
    void*           externContextPtr;   // Context pointer for the external stop handler.
}
ProcContainer_t;


//--------------------------------------------------------------------------------------------------
/**
 * The memory pool for process container objects.
 */
//--------------------------------------------------------------------------------------------------
static le_mem_PoolRef_t ProcContainerPool;


//--------------------------------------------------------------------------------------------------
/**
 * Application kill type.
 */
//--------------------------------------------------------------------------------------------------
typedef enum
{
    KILL_SOFT,          ///< Requests the application to clean up and shutdown.
    KILL_HARD           ///< Kills the application ASAP.
}
KillType_t;


//--------------------------------------------------------------------------------------------------
/**
 * Create the supplementary groups for an application.
 *
 * @todo Move creation of the groups to the installer.  Make this function just read the groups
 *       list into the app object.
 **/
//--------------------------------------------------------------------------------------------------
static le_result_t CreateSupplementaryGroups
(
    app_Ref_t appRef        // The app to create groups for.
)
{
    // Get an iterator to the supplementary groups list in the config.
    le_cfg_IteratorRef_t cfgIter = le_cfg_CreateReadTxn(appRef->cfgPathRoot);

    le_cfg_GoToNode(cfgIter, CFG_NODE_GROUPS);

    if (le_cfg_GoToFirstChild(cfgIter) != LE_OK)
    {
        LE_DEBUG("No supplementary groups for app '%s'.", appRef->name);
        le_cfg_CancelTxn(cfgIter);

        return LE_OK;
    }

    // Read the supplementary group names from the config.
    size_t i;
    for (i = 0; i < LIMIT_MAX_NUM_SUPPLEMENTARY_GROUPS; i++)
    {
        // Read the supplementary group name from the config.
        char groupName[LIMIT_MAX_USER_NAME_BYTES];
        if (le_cfg_GetNodeName(cfgIter, "", groupName, sizeof(groupName)) != LE_OK)
        {
            LE_ERROR("Could not read supplementary group for app '%s'.", appRef->name);
            le_cfg_CancelTxn(cfgIter);
            return LE_FAULT;
        }

        // Create the group.
        gid_t gid;
        if (user_CreateGroup(groupName, &gid) == LE_FAULT)
        {
            LE_ERROR("Could not create supplementary group '%s'.", groupName);
            le_cfg_CancelTxn(cfgIter);
            return LE_FAULT;
        }

        // Store the group id in the user's buffer.
        appRef->supplementGids[i] = gid;

        // Go to the next group.
        if (le_cfg_GoToNextSibling(cfgIter) != LE_OK)
        {
            break;
        }
        else if (i >= LIMIT_MAX_NUM_SUPPLEMENTARY_GROUPS - 1)
        {
            LE_ERROR("Too many supplementary groups for app '%s'.", appRef->name);
            le_cfg_CancelTxn(cfgIter);
            return LE_FAULT;
        }
    }

    appRef->numSupplementGids = i + 1;

    le_cfg_CancelTxn(cfgIter);

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Creates the user and groups in the /etc/passwd and /etc/groups files for an application.  This
 * function sets the uid and primary gid for the appRef and also populates the appRef's
 * supplementary groups list for non-sandboxed apps.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t CreateUserAndGroups
(
    app_Ref_t appRef        // The app to create user and groups for.
)
{
    // For sandboxed apps,
    if (appRef->sandboxed)
    {
        // Compute the unique user name for the application.
        char username[LIMIT_MAX_USER_NAME_BYTES];

        if (user_AppNameToUserName(appRef->name, username, sizeof(username)) != LE_OK)
        {
            LE_ERROR("The user name '%s' is too long.", username);
            return LE_FAULT;
        }

        // Get the user ID and primary group ID for this app.
        if (user_GetIDs(username, &(appRef->uid), &(appRef->gid)) != LE_OK)
        {
            LE_ERROR("Could not get uid and gid for user '%s'.", username);
            return LE_FAULT;
        }

        // Create the supplementary groups...
        return CreateSupplementaryGroups(appRef);
    }
    // For unsandboxed apps,
    else
    {
        // The user and group will be "root" (0).
        appRef->uid = 0;
        appRef->gid = 0;

        return LE_OK;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the configured permissions for a device.  The permissions will be returned in the provided
 * buffer as a string (either "r", "w" or "rw").  The provided buffer must be greater than or equal
 * to MAX_DEVICE_PERM_STR_BYTES bytes long.
 **/
//--------------------------------------------------------------------------------------------------
static void GetCfgPermissions
(
    le_cfg_IteratorRef_t cfgIter,       ///< [IN] Config iterator pointing to the device file.
    char* bufPtr,                       ///< [OUT] Buffer to hold the permission string.
    size_t bufSize                      ///< [IN] Size of the buffer.
)
{
    LE_FATAL_IF(bufSize < MAX_DEVICE_PERM_STR_BYTES,
                "Buffer size for permission string too small.");

    int i = 0;

    if (le_cfg_GetBool(cfgIter, "isReadable", false))
    {
        bufPtr[i++] = 'r';
    }

    if (le_cfg_GetBool(cfgIter, "isWritable", false))
    {
        bufPtr[i++] = 'w';
    }

    bufPtr[i] = '\0';
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the source path for the device file at the current node in the config iterator.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t GetDevSrcPath
(
    app_Ref_t appRef,                   ///< [IN] Reference to the application object.
    le_cfg_IteratorRef_t cfgIter,       ///< [IN] Config iterator for the import.
    char* bufPtr,                       ///< [OUT] Buffer to store the source path.
    size_t bufSize                      ///< [IN] Size of the buffer.
)
{
    char srcPath[LIMIT_MAX_PATH_BYTES] = "";

    if (le_cfg_GetString(cfgIter, "src", srcPath, sizeof(srcPath), "") != LE_OK)
    {
        LE_ERROR("Source file path '%s...' for app '%s' is too long.", srcPath, app_GetName(appRef));
        return LE_FAULT;
    }

    if (strlen(srcPath) == 0)
    {
        LE_ERROR("Empty source file path supplied for app %s.", app_GetName(appRef));
        return LE_FAULT;
    }

    if (le_utf8_Copy(bufPtr, srcPath, bufSize, NULL) != LE_OK)
    {
        LE_ERROR("Source file path '%s...' for app '%s' is too long.", srcPath, app_GetName(appRef));
        return LE_FAULT;
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets the device ID of a device file.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t GetDevID
(
    const char* fileNamePtr,        ///< [IN] Absolute path of the file.
    dev_t* idPtr                    ///< [OUT] Device ID.
)
{
    struct stat fileStat;

    if (stat(fileNamePtr, &fileStat) != 0)
    {
        LE_ERROR("Could not get file info for '%s'.  %m.", fileNamePtr);
        return LE_FAULT;
    }

    if (!S_ISCHR(fileStat.st_mode) && !S_ISBLK(fileStat.st_mode))
    {
        LE_ERROR("'%s' is not a device file.  %m.", fileNamePtr);
        return LE_FAULT;
    }

    *idPtr = fileStat.st_rdev;
    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets DAC and SMACK permissions for device files needed by this app.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t SetDevicePermissions
(
    app_Ref_t appRef                ///< [IN] The application.
)
{
    // Create an iterator for the app.
    le_cfg_IteratorRef_t appCfg = le_cfg_CreateReadTxn(app_GetConfigPath(appRef));

    // Get the list of device files.
    le_cfg_GoToNode(appCfg, CFG_NODE_REQUIRES);
    le_cfg_GoToNode(appCfg, CFG_NODE_DEVICES);

    if (le_cfg_GoToFirstChild(appCfg) == LE_OK)
    {
        do
        {
            // Get source path.
            char srcPath[LIMIT_MAX_PATH_BYTES];
            if (GetDevSrcPath(appRef, appCfg, srcPath, sizeof(srcPath)) != LE_OK)
            {
                le_cfg_CancelTxn(appCfg);
                return LE_FAULT;
            }

            // Check that the source is a device file.
            dev_t devId;

            if (GetDevID(srcPath, &devId) != LE_OK)
            {
                le_cfg_CancelTxn(appCfg);
                return LE_FAULT;
            }

            // TODO: Disallow device files that are security risks, such as block flash devices.

            // Assign a SMACK label to the device file.
            char devLabel[LIMIT_MAX_SMACK_LABEL_BYTES];
            le_result_t result = devSmack_GetLabel(devId, devLabel, sizeof(devLabel));

            LE_FATAL_IF(result == LE_OVERFLOW, "Smack label '%s...' too long.", devLabel);

            if (result != LE_OK)
            {
                le_cfg_CancelTxn(appCfg);
                return LE_FAULT;
            }

            if (smack_SetLabel(srcPath, devLabel) != LE_OK)
            {
                le_cfg_CancelTxn(appCfg);
                return LE_FAULT;
            }

            // Get the app's SMACK label.
            char appLabel[LIMIT_MAX_SMACK_LABEL_BYTES];
            smack_GetAppLabel(app_GetName(appRef), appLabel, sizeof(appLabel));

            // Get the required permissions for the device.
            char permStr[MAX_DEVICE_PERM_STR_BYTES];
            GetCfgPermissions(appCfg, permStr, sizeof(permStr));

            // Set the SMACK rule to allow the app to access the device.
            smack_SetRule(appLabel, permStr, devLabel);

            // Set the DAC permissions to be permissive.
            LE_FATAL_IF(chmod(srcPath, S_IROTH | S_IWOTH) == -1,
                        "Could not set permissions for file '%s'.  %m.", srcPath);
        }
        while (le_cfg_GoToNextSibling(appCfg) == LE_OK);

        le_cfg_GoToParent(appCfg);
    }

    le_cfg_CancelTxn(appCfg);

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets SMACK rules for an application based on its bindings.
 */
//--------------------------------------------------------------------------------------------------
static void SetSmackRulesForBindings
(
    app_Ref_t appRef,                   ///< [IN] Reference to the application.
    const char* appLabelPtr             ///< [IN] Smack label for the app.
)
{
    // Create a config read transaction to the bindings section for the application.
    le_cfg_IteratorRef_t bindCfg = le_cfg_CreateReadTxn(appRef->cfgPathRoot);
    le_cfg_GoToNode(bindCfg, CFG_NODE_BINDINGS);

    // Search the binding sections for server applications we need to set rules for.
    if (le_cfg_GoToFirstChild(bindCfg) != LE_OK)
    {
        // No bindings.
        le_cfg_CancelTxn(bindCfg);
    }

    do
    {
        char serverName[LIMIT_MAX_APP_NAME_BYTES];

        if ( (le_cfg_GetString(bindCfg, "app", serverName, sizeof(serverName), "") == LE_OK) &&
             (strcmp(serverName, "") != 0) )
        {
            // Get the server's SMACK label.
            char serverLabel[LIMIT_MAX_SMACK_LABEL_BYTES];
            smack_GetAppLabel(serverName, serverLabel, sizeof(serverLabel));

            // Set the SMACK label to/from the server.
            smack_SetRule(appLabelPtr, "rw", serverLabel);
            smack_SetRule(serverLabel, "rw", appLabelPtr);
        }
    } while (le_cfg_GoToNextSibling(bindCfg) == LE_OK);

    le_cfg_CancelTxn(bindCfg);
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets SMACK rules for an application and its folders.
 */
//--------------------------------------------------------------------------------------------------
static void SetDefaultSmackRules
(
    const char* appNamePtr,             ///< [IN] App name.
    const char* appLabelPtr             ///< [IN] Smack label for the app.
)
{
#define NUM_PERMISSONS      7

    const char* permissionStr[NUM_PERMISSONS] = {"x", "w", "wx", "r", "rx", "rw", "rwx"};

    // Set the rules for the app to access its own folders.
    int i;

    for (i = 0; i < NUM_PERMISSONS; i++)
    {
        // Create the mode from the permissions.
        mode_t mode = 0;

        if (strstr(permissionStr[i], "r") != NULL)
        {
            mode |= S_IRUSR;
        }
        if (strstr(permissionStr[i], "w") != NULL)
        {
            mode |= S_IWUSR;
        }
        if (strstr(permissionStr[i], "x") != NULL)
        {
            mode |= S_IXUSR;
        }

        char dirLabel[LIMIT_MAX_SMACK_LABEL_BYTES];
        smack_GetAppAccessLabel(appNamePtr, mode, dirLabel, sizeof(dirLabel));

        smack_SetRule(appLabelPtr, permissionStr[i], dirLabel);
    }

    // Set default permissions between the app and the framework.
    smack_SetRule("framework", "w", appLabelPtr);
    smack_SetRule(appLabelPtr, "rw", "framework");

    // Set default permissions to allow the app to access the syslog.
    smack_SetRule(appLabelPtr, "w", "syslog");
}


//--------------------------------------------------------------------------------------------------
/**
 * Cleans up all SMACK permissions for a given app.
 **/
//--------------------------------------------------------------------------------------------------
static void CleanupAppSmackSettings
(
    app_Ref_t appRef                    ///< [IN] The application reference.
)
{
    // Clean up SMACK rules.
    char appLabel[LIMIT_MAX_SMACK_LABEL_BYTES];
    smack_GetAppLabel(appRef->name, appLabel, sizeof(appLabel));

    smack_RevokeSubject(appLabel);
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets SMACK rules for an application.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t SetSmackRules
(
    app_Ref_t appRef                    ///< [IN] Reference to the application.
)
{
    // Clear out any residual SMACK rules from a previous incarnation of the Legato framework,
    // in case it wasn't shut down cleanly.
    CleanupAppSmackSettings(appRef);

    // Get the app label.
    char appLabel[LIMIT_MAX_SMACK_LABEL_BYTES];
    smack_GetAppLabel(appRef->name, appLabel, sizeof(appLabel));

    SetDefaultSmackRules(appRef->name, appLabel);

    SetSmackRulesForBindings(appRef, appLabel);

    return SetDevicePermissions(appRef);
}


//--------------------------------------------------------------------------------------------------
/**
 * Kills all the processes in the specified application.
 *
 * @return
 *      LE_OK if successful.
 *      LE_NOT_FOUND if there are no running processes in the app.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t KillAppProcs
(
    app_Ref_t appRef,       ///< [IN] Reference to the application whose processes should be killed.
    KillType_t  killType    ///< [IN] The type of kill to perform.
)
{
    // Freeze app procs.
    if (cgrp_frz_Freeze(appRef->name) == LE_OK)
    {
        // Wait till procs are frozen.
        while (1)
        {
            cgrp_FreezeState_t freezeState = cgrp_frz_GetState(appRef->name);

            if (freezeState == CGRP_FROZEN)
            {
                break;
            }
            else if ((le_result_t)freezeState == LE_FAULT)
            {
                LE_ERROR("Could not get freeze state of application '%s'.", appRef->name);
                break;
            }
        }

        LE_DEBUG("App '%s' frozen.", appRef->name);
    }
    else
    {
        LE_ERROR("Could not freeze processes for application '%s'.", appRef->name);
    }

    // Tell the child process objects we are going to kill them.
    le_dls_Link_t* procLinkPtr = le_dls_Peek(&(appRef->procs));

    while (procLinkPtr != NULL)
    {
        ProcContainer_t* procContainerPtr = CONTAINER_OF(procLinkPtr, ProcContainer_t, link);

        if (proc_GetState(procContainerPtr->procRef) != PROC_STATE_STOPPED)
        {
            procContainerPtr->stopHandler = NULL;
            proc_Stopping(procContainerPtr->procRef);
        }

        procLinkPtr = le_dls_PeekNext(&(appRef->procs), procLinkPtr);
    }

    // Kill all procs in the app including child processes and forked processes.
    int killSig = (killType == KILL_SOFT) ? SIGTERM: SIGKILL;

    ssize_t numProcs = cgrp_SendSig(CGRP_SUBSYS_FREEZE, appRef->name, killSig);

    if (numProcs == LE_FAULT)
    {
        LE_ERROR("Could not kill processes for application '%s'.", appRef->name);
        return LE_NOT_FOUND;
    }

    // Thaw app procs to allow them to run and process the signal we sent them.
    if (cgrp_frz_Thaw(appRef->name) != LE_OK)
    {
        LE_ERROR("Could not thaw processes for application '%s'.", appRef->name);
    }

    if (numProcs == 0)
    {
        return LE_NOT_FOUND;
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Performs a hard kill of all the processes in the specified application.  This function should be
 * called when the soft kill timeout expires.
 */
//--------------------------------------------------------------------------------------------------
static void HardKillApp
(
    le_timer_Ref_t timerRef
)
{
    app_Ref_t appRef = (app_Ref_t)le_timer_GetContextPtr(timerRef);

    LE_WARN("Hard killing app '%s'", appRef->name);

    KillAppProcs(appRef, KILL_HARD);
}


//--------------------------------------------------------------------------------------------------
/**
 * Finds a process container with this pid in the specified list.
 *
 * @return
 *      The pointer to a process container if successful.
 *      NULL if the process could not be found.
 */
//--------------------------------------------------------------------------------------------------
static ProcContainer_t* FindProcContainerInList
(
    le_dls_List_t list,             ///< [IN] List of process containers.
    pid_t pid                       ///< [IN] The pid to search for.
)
{
    le_dls_Link_t* procLinkPtr = le_dls_Peek(&list);

    while (procLinkPtr != NULL)
    {
        ProcContainer_t* procContainerPtr = CONTAINER_OF(procLinkPtr, ProcContainer_t, link);

        if (proc_GetPID(procContainerPtr->procRef) == pid)
        {
            return procContainerPtr;
        }

        procLinkPtr = le_dls_PeekNext(&list, procLinkPtr);
    }

    return NULL;
}


//--------------------------------------------------------------------------------------------------
/**
 * Finds a process container for the app by pid.
 *
 * @return
 *      The pointer to a process container if successful.
 *      NULL if the process could not be found.
 */
//--------------------------------------------------------------------------------------------------
static ProcContainer_t* FindProcContainer
(
    app_Ref_t appRef,               ///< [IN] The application to search in.
    pid_t pid                       ///< [IN] The pid to search for.
)
{
    // Find the process in the app's list.
    ProcContainer_t* procContainerPtr = FindProcContainerInList(appRef->procs, pid);

    if (procContainerPtr != NULL)
    {
        return procContainerPtr;
    }

    return FindProcContainerInList(appRef->auxProcs, pid);
}


//--------------------------------------------------------------------------------------------------
/**
 * Checks if the application has any processes running.
 *
 * @note This only applies to child processes.  Forked processes in the application are not
 *       monitored.
 *
 * @return
 *      true if there is at least one running process for the application.
 *      false if there are no running processes for the application.
 */
//--------------------------------------------------------------------------------------------------
static bool HasRunningProc
(
    app_Ref_t appRef                    ///< [IN] The application reference.
)
{
    return !cgrp_IsEmpty(CGRP_SUBSYS_FREEZE, appRef->name);
}


//--------------------------------------------------------------------------------------------------
/**
 * Stops the specified process.
 */
//--------------------------------------------------------------------------------------------------
static void StopProc
(
    proc_Ref_t procRef
)
{
    proc_Stopping(procRef);

    pid_t pid = proc_GetPID(procRef);

    kill_Hard(pid);
}


//--------------------------------------------------------------------------------------------------
/**
 * Create the sandbox app's /tmp folder and mount a tmpfs at that location.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t CreateTmpFs
(
    app_Ref_t appRef,                   ///< [IN] Application reference.
    const char* appDirLabelPtr          ///< [IN] SMACK label to use for created directories.
)
{
// TODO: make this configurable.
#define APP_TMPFS_SIZE      90112       // Bytes.

    // Create /tmp folder in the sandbox.
    char tmpPath[LIMIT_MAX_PATH_BYTES] = "";

    if (le_path_Concat("/", tmpPath, sizeof(tmpPath), appRef->workingDir, "tmp", NULL) != LE_OK)
    {
        LE_ERROR("Path '%s...' is too long.", tmpPath);
        return LE_FAULT;
    }

    if (dir_MakeSmack(tmpPath, S_IRWXO, appDirLabelPtr) == LE_FAULT)
    {
        return LE_FAULT;
    }

    // Make the mount options.
    char opt[LIMIT_MAX_APP_NAME_BYTES*2 + 100];
    if (snprintf(opt, sizeof(opt), "size=%d,mode=%.4o,uid=%d,gid=%d,smackfsdef=%s,smackfsroot=%s",
                 APP_TMPFS_SIZE, S_IRWXO, 0, 0,
                 appDirLabelPtr, appDirLabelPtr) >= sizeof(opt))
    {
        LE_ERROR("Mount options string is too long. '%s'", opt);

        return LE_FAULT;
    }

    // Unmount any previously mounted file system.
    fs_TryLazyUmount(tmpPath);

    // Mount the tmpfs for the sandbox.
    if (mount("tmpfs", tmpPath, "tmpfs", MS_NOSUID, opt) == -1)
    {
        LE_ERROR("Could not mount tmpfs for sandbox '%s'.  %m.", app_GetName(appRef));

        return LE_FAULT;
    }

    LE_INFO("Mounted tmpfs at %s.", tmpPath);

    return smack_SetLabel(tmpPath, appDirLabelPtr);
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets the absolute destination path.  If the destination path ends with a '/' then the last node
 * of the source is appended to the destination.
 *
 * @return
 *      LE_OK if successful.
 *      LE_OVERFLOW the provided buffer is too small.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t GetAbsDestPath
(
    const char* destPtr,        ///< [IN] Relative destination path.
    const char* srcPtr,         ///< [IN] Source path.
    const char* appRunDir,      ///< [IN] Path to the app's runtime area.
    char* bufPtr,               ///< [OUT] Buffer to store the final destination path.
    size_t bufSize              ///< [IN] Size of the buffer.
)
{
    bufPtr[0] = '\0';

    if (destPtr[strlen(destPtr)-1] == '/')
    {
        return le_path_Concat("/", bufPtr, bufSize, appRunDir, destPtr,
                              le_path_GetBasenamePtr(srcPtr, "/"), NULL);
    }
    else
    {
        return le_path_Concat("/", bufPtr, bufSize, appRunDir, destPtr, NULL);
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Creates all intermediate directories along the path.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t CreateIntermediateDirs
(
    const char* pathPtr,                ///< [IN] Path.
    const char* smackLabelPtr           ///< [IN] SMACK label to use for the created dirs.
)
{
    char dirPath[LIMIT_MAX_PATH_BYTES] = "";

    if (le_path_GetDir(pathPtr, "/", dirPath, sizeof(dirPath)) != LE_OK)
    {
        LE_ERROR("Path '%s' is too long.", dirPath);
        return LE_FAULT;
    }

    if (dir_MakePathSmack(dirPath,
                          S_IRUSR | S_IXUSR | S_IROTH | S_IXOTH,
                          smackLabelPtr) == LE_FAULT)
    {
        return LE_FAULT;
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Check if the link already exists.
 *
 * If there is a link to a different file then attempt to delete it.
 *
 * @return
 *      true if link already exists.
 *      false otherwise.
 */
//--------------------------------------------------------------------------------------------------
static bool DoesLinkExist
(
    app_Ref_t appRef,                   ///< [IN] Application reference.
    struct stat srcStat,                ///< [IN] Status of the source.
    const char* destPath                ///< [IN] Destination path.
)
{
    // See if the destination already exists.
    struct stat destStat;

    if (stat(destPath, &destStat) == -1)
    {
        if  (errno != ENOENT)
        {
            LE_WARN("Could not stat file at '%s'. %m", destPath);
        }
    }
    else
    {
        // Destination file already exists.  See if it has changed.
        if (srcStat.st_ino == destStat.st_ino)
        {
            // Link already exists.
            return true;
        }

        // Attempt to delete the original link.
        if (!appRef->sandboxed)
        {
            if (unlink(destPath) == -1)
            {
                LE_WARN("Could not delete %s.  %m,", destPath);
            }
        }
    }

    return false;
}


//--------------------------------------------------------------------------------------------------
/**
 * Create a directory link from the source to the destination.  The source is always assumed to be
 * an absolute path while the destination is relative to the application runtime area.  If the
 * destination includes directories that do not exist then those directories are created.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t CreateDirLink
(
    app_Ref_t appRef,                   ///< [IN] Application reference.
    const char* appDirLabelPtr,         ///< [IN] SMACK label to use for created directories.
    const char* srcPtr,                 ///< [IN] Source path.
    const char* destPtr                 ///< [IN] Destination path.
)
{
    // Check the source.
    struct stat srcStat;

    if (stat(srcPtr, &srcStat) == -1)
    {
        LE_ERROR("Could not stat file at '%s'. %m", srcPtr);
        return LE_FAULT;
    }

    if (!S_ISDIR(srcStat.st_mode))
    {
        LE_ERROR("'%s' is not a directory.", srcPtr);
        return LE_FAULT;
    }

    // Get the absolute destination path.
    char destPath[LIMIT_MAX_PATH_BYTES] = "";

    if (GetAbsDestPath(destPtr, srcPtr, appRef->workingDir, destPath, sizeof(destPath)) != LE_OK)
    {
        LE_ERROR("Link destination path '%s' is too long.", destPath);
        return LE_FAULT;
    }

    // Create the necessary intermediate directories along the destination path.
    if (CreateIntermediateDirs(destPath, appDirLabelPtr) != LE_OK)
    {
        return LE_FAULT;
    }

    // See if the destination already exists.
    if (DoesLinkExist(appRef, srcStat, destPath))
    {
        return LE_OK;
    }

    // Create the link.
    if (appRef->sandboxed)
    {
        // Make the destination directories.
        if (dir_MakeSmack(destPath,
                      S_IRUSR | S_IXUSR | S_IROTH | S_IXOTH,
                      appDirLabelPtr) == LE_FAULT)
        {
            return LE_FAULT;
        }

        // Bind mount file into the sandbox.
        if (mount(srcPtr, destPath, NULL, MS_BIND, NULL) != 0)
        {
            LE_ERROR("Couldn't bind mount from '%s' to '%s'. %m", srcPtr, destPath);
            return LE_FAULT;
        }
    }
    else
    {
        // Create a symlink at the specified path.
        if (symlink(srcPtr, destPath) != 0)
        {
            LE_ERROR("Could not create symlink from '%s' to '%s'. %m", srcPtr, destPath);
            return LE_FAULT;
        }
    }

    LE_INFO("Created directory link '%s' to '%s'.", srcPtr, destPath);

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Create a file link from the source to the destination.  The source is always assumed to be
 * an absolute path while the destination is relative to the application runtime area.  If the
 * destination includes directories that do not exist then those directories are created.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t CreateFileLink
(
    app_Ref_t appRef,                   ///< [IN] Application reference.
    const char* appDirLabelPtr,         ///< [IN] SMACK label to use for created directories.
    const char* srcPtr,                 ///< [IN] Source path.
    const char* destPtr                 ///< [IN] Destination path.
)
{
    // Check the source.
    struct stat srcStat;

    if (stat(srcPtr, &srcStat) == -1)
    {
        LE_ERROR("Could not stat file at '%s'. %m", srcPtr);
        return LE_FAULT;
    }

    if (S_ISDIR(srcStat.st_mode))
    {
        LE_ERROR("'%s' is a directory.", srcPtr);
        return LE_FAULT;
    }

    // Get the absolute destination path.
    char destPath[LIMIT_MAX_PATH_BYTES] = "";

    if (GetAbsDestPath(destPtr, srcPtr, appRef->workingDir, destPath, sizeof(destPath)) != LE_OK)
    {
        LE_ERROR("Link destination path '%s' is too long.", destPath);
        return LE_FAULT;
    }

    // Create the necessary intermediate directories along the destination path.
    if (CreateIntermediateDirs(destPath, appDirLabelPtr) != LE_OK)
    {
        return LE_FAULT;
    }

    // See if the destination already exists.
    if (DoesLinkExist(appRef, srcStat, destPath))
    {
        return LE_OK;
    }

    // Create the link.
    if (appRef->sandboxed)
    {
        // Create an empty file at the specified path.
        int fd;
        while ( ((fd = open(destPath, O_RDONLY | O_CREAT, S_IRUSR)) == -1) && (errno == EINTR) ) {}

        if (fd == -1)
        {
            LE_ERROR("Could not create file '%s'.  %m", destPath);
            return LE_FAULT;
        }

        fd_Close(fd);

        // Bind mount file into the sandbox.
        if (mount(srcPtr, destPath, NULL, MS_BIND, NULL) != 0)
        {
            LE_ERROR("Couldn't bind mount from '%s' to '%s'. %m", srcPtr, destPath);
            return LE_FAULT;
        }
    }
    else
    {
        // Create a symlink at the specified path.
        if (symlink(srcPtr, destPath) != 0)
        {
            LE_ERROR("Could not create symlink from '%s' to '%s'. %m", srcPtr, destPath);
            return LE_FAULT;
        }
    }

    LE_INFO("Created file link '%s' to '%s'.", srcPtr, destPath);

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Recursively create links from all files under the source directory to corresponding files under
 * the destination directory.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t RecursivelyCreateLinks
(
    app_Ref_t appRef,                   ///< [IN] Application reference.
    const char* appDirLabelPtr,         ///< [IN] SMACK label to use for created directories.
    const char* srcDirPtr,              ///< [IN] Source directory.
    const char* destDirPtr              ///< [IN] Destination directory.
)
{
    char baseDestPath[LIMIT_MAX_PATH_BYTES] = "";

    if (destDirPtr[strlen(destDirPtr)-1] == '/')
    {
        // Use the source directory name in the destination.
        if (le_path_Concat("/", baseDestPath, sizeof(baseDestPath), destDirPtr,
                            le_path_GetBasenamePtr(srcDirPtr, "/"), NULL) != LE_OK)
        {
            LE_ERROR("Destination path '%s...' for app %s is too long.", baseDestPath, appRef->name);
            return LE_FAULT;
        }
    }
    else
    {
        if (le_utf8_Copy(baseDestPath, destDirPtr, sizeof(baseDestPath), NULL) != LE_OK)
        {
            LE_ERROR("Destination path '%s...' for app %s is too long.", baseDestPath, appRef->name);
            return LE_FAULT;
        }
    }

    // Open the directory tree to search.
    char* pathArrayPtr[] = {(char*)srcDirPtr, NULL};

    FTS* ftsPtr = NULL;
    errno = 0;

    do
    {
        if (appRef->sandboxed)
        {
            ftsPtr = fts_open(pathArrayPtr, FTS_LOGICAL | FTS_NOSTAT, NULL);
        }
        else
        {
            ftsPtr = fts_open(pathArrayPtr, FTS_PHYSICAL | FTS_NOSTAT, NULL);
        }
    }
    while ( (ftsPtr == NULL) && (errno == EINTR) );

    if (ftsPtr == NULL)
    {
        LE_ERROR("Could open directory '%s'.  %m.", srcDirPtr);
        return LE_FAULT;
    }

    // Step through the directory tree.
    FTSENT* srcEntPtr;

    int srcDirLen = strlen(srcDirPtr);

    while ((srcEntPtr = fts_read(ftsPtr)) != NULL)
    {
        switch (srcEntPtr->fts_info)
        {
            case FTS_SL:
            case FTS_F:
            case FTS_NSOK:
            {
                // These are files and links.
                // Create the dest path.
                char destPath[LIMIT_MAX_PATH_BYTES] = "";

                if (le_path_Concat("/", destPath, sizeof(destPath), baseDestPath,
                                   srcEntPtr->fts_path + srcDirLen, NULL) != LE_OK)
                {
                    fts_close(ftsPtr);
                    LE_ERROR("Full destination path '%s...' for app %s is too long.", destPath,
                             appRef->name);
                    return LE_FAULT;
                }

                // Create the link.
                if (CreateFileLink(appRef, appDirLabelPtr, srcEntPtr->fts_path, destPath) != LE_OK)
                {
                    fts_close(ftsPtr);
                    return LE_FAULT;
                }
            }
        }
    }

    int lastErrno = errno;

    // Close the directory tree.
    int r;
    do
    {
        r = fts_close(ftsPtr);
    }
    while ( (r == -1) && (errno == EINTR) );

    if (lastErrno != 0)
    {
        LE_ERROR("Could not read directory '%s'.  %m", srcDirPtr);
        return LE_FAULT;
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Create links to the default temporary files that all app's will likely need.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t CreateDefaultTmpLinks
(
    app_Ref_t appRef,                   ///< [IN] Application reference.
    const char* appDirLabelPtr          ///< [IN] SMACK label to use for created directories.
)
{
    int i = 0;

    for (i = 0; i < NUM_ARRAY_MEMBERS(DefaultTmpLinks); i++)
    {
        // Default links must work otherwise there is something very wrong.
        if (CreateFileLink(appRef, appDirLabelPtr,
                           DefaultTmpLinks[i].src, DefaultTmpLinks[i].dest) != LE_OK)
        {
            return LE_FAULT;
        }
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Create links to the default libs and files that all app's will likely need.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t CreateDefaultLinks
(
    app_Ref_t appRef,                   ///< [IN] Application reference.
    const char* appDirLabelPtr          ///< [IN] SMACK label to use for created directories.
)
{
    int i = 0;

    for (i = 0; i < NUM_ARRAY_MEMBERS(DefaultLinks); i++)
    {
        // Default links must work otherwise there is something very wrong.
        if (CreateFileLink(appRef, appDirLabelPtr,
                           DefaultLinks[i].src, DefaultLinks[i].dest) != LE_OK)
        {
            return LE_FAULT;
        }
    }

    for (i = 0; i < NUM_ARRAY_MEMBERS(DefaultSystemLinks); i++)
    {
        // Default links must work otherwise there is something very wrong.
        if (CreateFileLink(appRef, appDirLabelPtr, DefaultSystemLinks[i].src,
                           DefaultSystemLinks[i].dest) != LE_OK)
        {
            return LE_FAULT;
        }
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Create links to the app's lib and bin files.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t CreateLibBinLinks
(
    app_Ref_t appRef,                   ///< [IN] Application reference.
    const char* appDirLabelPtr          ///< [IN] SMACK label to use for created directories.
)
{
    // Create links to the apps lib directory.
    char srcLib[LIMIT_MAX_PATH_BYTES] = "";

    if (le_path_Concat("/", srcLib, sizeof(srcLib), appRef->installDirPath, "read-only/lib", NULL)
          != LE_OK)
    {
        LE_ERROR("App's install dir path too long!");
        return LE_FAULT;
    }

    if (RecursivelyCreateLinks(appRef, appDirLabelPtr, srcLib, "/lib") != LE_OK)
    {
        return LE_FAULT;
    }

    // Create links to the apps bin directory.
    char srcBin[LIMIT_MAX_PATH_BYTES] = "";

    if (le_path_Concat("/", srcBin, sizeof(srcBin), appRef->installDirPath, "read-only/bin", NULL)
          != LE_OK)
    {
        LE_ERROR("App's install dir path too long!");
        return LE_FAULT;
    }

    if (RecursivelyCreateLinks(appRef, appDirLabelPtr, srcBin, "/bin") != LE_OK)
    {
        return LE_FAULT;
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the source path for read only bundled files at the current node in the config iterator.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t GetBundledReadOnlySrcPath
(
    app_Ref_t appRef,                   ///< [IN] Reference to the application object.
    le_cfg_IteratorRef_t cfgIter,       ///< [IN] Config iterator.
    char* bufPtr,                       ///< [OUT] Buffer to store the source path.
    size_t bufSize                      ///< [IN] Size of the buffer.
)
{
    char srcPath[LIMIT_MAX_PATH_BYTES] = "";

    if (le_cfg_GetString(cfgIter, "src", srcPath, sizeof(srcPath), "") != LE_OK)
    {
        LE_ERROR("Source file path '%s...' for app '%s' is too long.", srcPath, app_GetName(appRef));
        return LE_FAULT;
    }

    if (strlen(srcPath) == 0)
    {
        LE_ERROR("Empty source file path supplied for app %s.", app_GetName(appRef));
        return LE_FAULT;
    }

    if (srcPath[0] == '/')
    {
        // The source path is an absolute path so just copy it to the user's buffer.
        if (le_utf8_Copy(bufPtr, srcPath, bufSize, NULL) != LE_OK)
        {
            LE_ERROR("Source file path '%s...' for app '%s' is too long.", srcPath, app_GetName(appRef));
            return LE_FAULT;
        }
    }
    else
    {
        // The source file path is relative to the app install directory.
        bufPtr[0] = '\0';
        if (LE_OK != le_path_Concat("/",
                                    bufPtr,
                                    bufSize,
                                    appRef->installDirPath,
                                    "read-only",
                                    srcPath,
                                    NULL) )
        {
            LE_ERROR("Import source path '%s' for app '%s' is too long.", bufPtr, app_GetName(appRef));
            return LE_FAULT;
        }
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the destination path for the app at the current node in the config iterator.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t GetDestPath
(
    app_Ref_t appRef,                   ///< [IN] Reference to the application object.
    le_cfg_IteratorRef_t cfgIter,       ///< [IN] Config iterator.
    char* bufPtr,                       ///< [OUT] Buffer to store the path.
    size_t bufSize                      ///< [IN] Size of the buffer.
)
{
    if (le_cfg_GetString(cfgIter, "dest", bufPtr, bufSize, "") != LE_OK)
    {
        LE_ERROR("Destination path '%s...' for app '%s' is too long.", bufPtr, appRef->name);
        return LE_FAULT;
    }

    if (bufPtr[0] == '\0')
    {
        LE_ERROR("Empty dest path supplied for app %s.", appRef->name);
        return LE_FAULT;
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the source path for the app at the current node in the config iterator.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t GetSrcPath
(
    app_Ref_t appRef,                   ///< [IN] Reference to the application object.
    le_cfg_IteratorRef_t cfgIter,       ///< [IN] Config iterator.
    char* bufPtr,                       ///< [OUT] Buffer to store the path.
    size_t bufSize                      ///< [IN] Size of the buffer.
)
{
    if (le_cfg_GetString(cfgIter, "src", bufPtr, bufSize, "") != LE_OK)
    {
        LE_ERROR("Source path '%s...' for app '%s' is too long.", bufPtr, appRef->name);
        return LE_FAULT;
    }

    if (bufPtr[0] == '\0')
    {
        LE_ERROR("Empty src path supplied for app %s.", appRef->name);
        return LE_FAULT;
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Create links to the app's read only bundled files.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t CreateBundledLinks
(
    app_Ref_t appRef,                   ///< [IN] Application reference.
    const char* appDirLabelPtr          ///< [IN] SMACK label to use for created directories.
)
{
    // Get a config iterator for this app.
    le_cfg_IteratorRef_t appCfg = le_cfg_CreateReadTxn(appRef->cfgPathRoot);

    // Go to the bundled directories section.
    le_cfg_GoToNode(appCfg, CFG_NODE_BUNDLES);
    le_cfg_GoToNode(appCfg, CFG_NODE_DIRS);

    if (le_cfg_GoToFirstChild(appCfg) == LE_OK)
    {
        do
        {
            // Only handle read only directories.
            if (!le_cfg_GetBool(appCfg, "isWritable", false))
            {
                // Get source path.
                char srcPath[LIMIT_MAX_PATH_BYTES];
                if (GetBundledReadOnlySrcPath(appRef, appCfg, srcPath, sizeof(srcPath)) != LE_OK)
                {
                    le_cfg_CancelTxn(appCfg);
                    return LE_FAULT;
                }

                // Get destination path.
                char destPath[LIMIT_MAX_PATH_BYTES];
                if (GetDestPath(appRef, appCfg, destPath, sizeof(destPath)) != LE_OK)
                {
                    le_cfg_CancelTxn(appCfg);
                    return LE_FAULT;
                }

                // Create links for all files in the source directory.
                if (RecursivelyCreateLinks(appRef, appDirLabelPtr, srcPath, destPath) != LE_OK)
                {
                    le_cfg_CancelTxn(appCfg);
                    return LE_FAULT;
                }
            }
        }
        while (le_cfg_GoToNextSibling(appCfg) == LE_OK);

        le_cfg_GoToParent(appCfg);
    }

    // Go to the requires files section.
    le_cfg_GoToParent(appCfg);
    le_cfg_GoToNode(appCfg, CFG_NODE_FILES);

    if (le_cfg_GoToFirstChild(appCfg) == LE_OK)
    {
        do
        {
            // Only handle read only files.
            if (!le_cfg_GetBool(appCfg, "isWritable", false))
            {
                // Get source path.
                char srcPath[LIMIT_MAX_PATH_BYTES];
                if (GetBundledReadOnlySrcPath(appRef, appCfg, srcPath, sizeof(srcPath)) != LE_OK)
                {
                    le_cfg_CancelTxn(appCfg);
                    return LE_FAULT;
                }

                // Get destination path.
                char destPath[LIMIT_MAX_PATH_BYTES];
                if (GetDestPath(appRef, appCfg, destPath, sizeof(destPath)) != LE_OK)
                {
                    le_cfg_CancelTxn(appCfg);
                    return LE_FAULT;
                }

                if (CreateFileLink(appRef, appDirLabelPtr, srcPath, destPath) != LE_OK)
                {
                    le_cfg_CancelTxn(appCfg);
                    return LE_FAULT;
                }
            }
        }
        while (le_cfg_GoToNextSibling(appCfg) == LE_OK);
    }

    le_cfg_CancelTxn(appCfg);

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Create links to the app's required files under the current node in the configuration iterator.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t CreateRequiredFileLinks
(
    app_Ref_t appRef,                   ///< [IN] Application reference.
    const char* appDirLabelPtr,         ///< [IN] SMACK label to use for created directories.
    le_cfg_IteratorRef_t cfgIter        ///< [IN] Config iterator.
)
{
    if (le_cfg_GoToFirstChild(cfgIter) == LE_OK)
    {
        do
        {
            // Get source path.
            char srcPath[LIMIT_MAX_PATH_BYTES];

            if (GetSrcPath(appRef, cfgIter, srcPath, sizeof(srcPath)) != LE_OK)
            {
                return LE_FAULT;
            }

            // Get destination path.
            char destPath[LIMIT_MAX_PATH_BYTES];
            if (GetDestPath(appRef, cfgIter, destPath, sizeof(destPath)) != LE_OK)
            {
                return LE_FAULT;
            }

            if (CreateFileLink(appRef, appDirLabelPtr, srcPath, destPath) != LE_OK)
            {
                return LE_FAULT;
            }
        }
        while (le_cfg_GoToNextSibling(cfgIter) == LE_OK);

        le_cfg_GoToParent(cfgIter);
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Create links to the app's required directories, files and devices.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t CreateRequiredLinks
(
    app_Ref_t appRef,                   ///< [IN] Application reference.
    const char* appDirLabelPtr          ///< [IN] SMACK label to use for created directories.
)
{
    // Get a config iterator for this app.
    le_cfg_IteratorRef_t appCfg = le_cfg_CreateReadTxn(appRef->cfgPathRoot);

    // Go to the required directories section.
    le_cfg_GoToNode(appCfg, CFG_NODE_REQUIRES);
    le_cfg_GoToNode(appCfg, CFG_NODE_DIRS);

    if (le_cfg_GoToFirstChild(appCfg) == LE_OK)
    {
        do
        {
            // Get source path.
            char srcPath[LIMIT_MAX_PATH_BYTES];

            if (GetSrcPath(appRef, appCfg, srcPath, sizeof(srcPath)) != LE_OK)
            {
                le_cfg_CancelTxn(appCfg);
                return LE_FAULT;
            }

            // Get destination path.
            char destPath[LIMIT_MAX_PATH_BYTES];
            if (GetDestPath(appRef, appCfg, destPath, sizeof(destPath)) != LE_OK)
            {
                le_cfg_CancelTxn(appCfg);
                return LE_FAULT;
            }

            // Treat /proc and /sys differently.  These are kernel file systems that user space
            // processes cannot write create files in.  So it is safe to create a link to the
            // entire directory.
            if ( le_path_IsEquivalent("/proc", srcPath, "/") ||
                 le_path_IsEquivalent("/sys", srcPath, "/") ||
                 le_path_IsSubpath("/proc", srcPath, "/") ||
                 le_path_IsSubpath("/sys", srcPath, "/") )
            {
                if (CreateDirLink(appRef, appDirLabelPtr, srcPath, destPath) != LE_OK)
                {
                    le_cfg_CancelTxn(appCfg);
                    return LE_FAULT;
                }
            }
            else
            {
                // Create links for all files in the source directory.
                if (RecursivelyCreateLinks(appRef, appDirLabelPtr, srcPath, destPath) != LE_OK)
                {
                    le_cfg_CancelTxn(appCfg);
                    return LE_FAULT;
                }
            }
        }
        while (le_cfg_GoToNextSibling(appCfg) == LE_OK);

        le_cfg_GoToParent(appCfg);
    }

    // Go to the requires files section
    le_cfg_GoToParent(appCfg);
    le_cfg_GoToNode(appCfg, CFG_NODE_FILES);

    if (CreateRequiredFileLinks(appRef, appDirLabelPtr, appCfg) != LE_OK)
    {
        le_cfg_CancelTxn(appCfg);
        return LE_FAULT;
    }

    // Go to the devices section.
    le_cfg_GoToParent(appCfg);
    le_cfg_GoToNode(appCfg, CFG_NODE_DEVICES);

    if (CreateRequiredFileLinks(appRef, appDirLabelPtr, appCfg) != LE_OK)
    {
        le_cfg_CancelTxn(appCfg);
        return LE_FAULT;
    }

    le_cfg_CancelTxn(appCfg);
    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets up the application execution area in the file system.  For a sandboxed app this will be the
 * sandbox.  For an unsandboxed app this will be the app's current working directory..
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t SetupAppArea
(
    app_Ref_t appRef                    ///< [IN] The application reference.
)
{
    // Get the SMACK label for the folders we create.
    char appDirLabel[LIMIT_MAX_SMACK_LABEL_BYTES];
    smack_GetAppAccessLabel(app_GetName(appRef), S_IRWXU, appDirLabel, sizeof(appDirLabel));

    // Create the appsWritable/<appName> directory if it does not already exist.
    if (dir_MakeSmack(appRef->workingDir,
                      S_IRUSR | S_IXUSR | S_IROTH | S_IWOTH | S_IXOTH,
                      appDirLabel) == LE_FAULT)
    {
        return LE_FAULT;
    }

    if (appRef->sandboxed)
    {
        if (!fs_IsMountPoint(appRef->workingDir))
        {
            // Bind mount the root of the sandbox unto itself so that we just lazy umount this when we
            // need to clean up.
            if (mount(appRef->workingDir, appRef->workingDir, NULL, MS_BIND, NULL) != 0)
            {
                LE_ERROR("Couldn't bind mount '%s' unto itself. %m", appRef->workingDir);
                return LE_FAULT;
            }
        }

        // Create default links.
        if (CreateDefaultLinks(appRef, appDirLabel) != LE_OK)
        {
            return LE_FAULT;
        }
    }

    // Create links to the app's lib and bin directories.
    if (CreateLibBinLinks(appRef, appDirLabel) != LE_OK)
    {
        return LE_FAULT;
    }

    // Create links to bundled files.
    if (CreateBundledLinks(appRef, appDirLabel) != LE_OK)
    {
        return LE_FAULT;
    }

    // Create links to required files.
    if (CreateRequiredLinks(appRef, appDirLabel) != LE_OK)
    {
        return LE_FAULT;
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Initialize the application system.
 */
//--------------------------------------------------------------------------------------------------
void app_Init
(
    void
)
{
    AppPool = le_mem_CreatePool("Apps", sizeof(App_t));
    ProcContainerPool = le_mem_CreatePool("ProcContainers", sizeof(ProcContainer_t));

    proc_Init();

    // Create the appsWriteable area.
    if (le_dir_MakePath(APPS_WRITEABLE_DIR, S_IRUSR | S_IXUSR | S_IROTH | S_IXOTH) != LE_OK)
    {
        LE_ERROR("Could not make appsWriteable dir, applications may not start.");
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets a process container for the app by name.
 *
 * @return
 *      The pointer to a process container if successful.
 *      NULL if the process container could not be found.
 */
//--------------------------------------------------------------------------------------------------
static ProcContainer_t* GetProcContainer
(
    app_Ref_t appRef,               ///< [IN] The application to search in.
    const char* procNamePtr         ///< [IN] The process name to get for.
)
{
    if (procNamePtr == NULL)
    {
        return NULL;
    }

    // Find the process in the app's list.
    le_dls_Link_t* procLinkPtr = le_dls_Peek(&(appRef->procs));

    while (procLinkPtr != NULL)
    {
        ProcContainer_t* procContainerPtr = CONTAINER_OF(procLinkPtr, ProcContainer_t, link);

        if (strcmp(procNamePtr, proc_GetName(procContainerPtr->procRef)) == 0)
        {
            return procContainerPtr;
        }

        procLinkPtr = le_dls_PeekNext(&(appRef->procs), procLinkPtr);
    }

    return NULL;
}


//--------------------------------------------------------------------------------------------------
/**
 * Creates a new process container.
 *
 * @return
 *      Process container reference.
 */
//--------------------------------------------------------------------------------------------------
static ProcContainer_t* CreateProcContainer
(
    app_Ref_t appRef,                   ///< [IN] Application this process belongs to.
    proc_Ref_t procRef                  ///< [IN] Process reference.
)
{
    ProcContainer_t* procContainerPtr = le_mem_ForceAlloc(ProcContainerPool);

    procContainerPtr->procRef = procRef;
    procContainerPtr->stopHandler = NULL;
    procContainerPtr->link = LE_DLS_LINK_INIT;
    procContainerPtr->externStopHandler = NULL;
    procContainerPtr->externContextPtr = NULL;

    return procContainerPtr;
}


//--------------------------------------------------------------------------------------------------
/**
 * Creates an application object.
 *
 * @note
 *      The name of the application is the node name (last part) of the cfgPathRootPtr.
 *
 * @return
 *      A reference to the application object if success.
 *      NULL if there was an error.
 */
//--------------------------------------------------------------------------------------------------
app_Ref_t app_Create
(
    const char* cfgPathRootPtr      ///< [IN] The path in the config tree for this application.
)
{
    // Create a new app object.
    App_t* appPtr = le_mem_ForceAlloc(AppPool);

    // Save the config path.
    if (   le_utf8_Copy(appPtr->cfgPathRoot, cfgPathRootPtr, sizeof(appPtr->cfgPathRoot), NULL)
        != LE_OK)
    {
        LE_ERROR("Config path '%s' is too long.", cfgPathRootPtr);

        le_mem_Release(appPtr);
        return NULL;
    }

    // Store the app name.
    appPtr->name = le_path_GetBasenamePtr(appPtr->cfgPathRoot, "/");

    // Initialize the other parameters.
    appPtr->procs = LE_DLS_LIST_INIT;
    appPtr->auxProcs = LE_DLS_LIST_INIT;
    appPtr->state = APP_STATE_STOPPED;
    appPtr->killTimer = NULL;

    // Get a config iterator for this app.
    le_cfg_IteratorRef_t cfgIterator = le_cfg_CreateReadTxn(appPtr->cfgPathRoot);

    // See if this is a sandboxed app.
    appPtr->sandboxed = le_cfg_GetBool(cfgIterator, CFG_NODE_SANDBOXED, true);

    // @todo: Create the user and all the groups for this app.  This function has a side affect
    //        where it populates the app's supplementary groups list and sets the uid and the
    //        primary gid.  This behaviour will be changed when the create user functionality is
    //        moved to the app installer.
    if (CreateUserAndGroups(appPtr) != LE_OK)
    {
        goto failed;
    }

    // Get the app's install directory path.
    appPtr->installDirPath[0] = '\0';
    if (LE_OK != le_path_Concat("/",
                                appPtr->installDirPath,
                                sizeof(appPtr->installDirPath),
                                APPS_INSTALL_DIR,
                                appPtr->name,
                                NULL))
    {
        LE_ERROR("Install directory path '%s' is too long.  App '%s' cannot be started.",
                 appPtr->installDirPath,
                 appPtr->name);
        goto failed;
    }

    // Use the app's writeable files' directory path as the its working directory.
    appPtr->workingDir[0] = '\0';
    if (LE_OK != le_path_Concat("/",
                                appPtr->workingDir,
                                sizeof(appPtr->workingDir),
                                APPS_WRITEABLE_DIR,
                                appPtr->name,
                                NULL))
    {
        LE_ERROR("Writeable files directory path '%s' is too long.  App '%s' cannot be started.",
                 appPtr->workingDir,
                 appPtr->name);
        goto failed;
    }

    // Move the config iterator to the procs list for this app.
    le_cfg_GoToNode(cfgIterator, CFG_NODE_PROC_LIST);

    // Read the list of processes for this application from the config tree.
    if (le_cfg_GoToFirstChild(cfgIterator) == LE_OK)
    {
        do
        {
            // Get the process's config path.
            char procCfgPath[LIMIT_MAX_PATH_BYTES];

            if (le_cfg_GetPath(cfgIterator, "", procCfgPath, sizeof(procCfgPath)) == LE_OVERFLOW)
            {
                LE_ERROR("Internal path buffer too small.");
                goto failed;
            }

            // Strip off the trailing '/'.
            size_t lastIndex = le_utf8_NumBytes(procCfgPath) - 1;

            if (procCfgPath[lastIndex] == '/')
            {
                procCfgPath[lastIndex] = '\0';
            }

            // Get the process name.
            char* procNamePtr = le_path_GetBasenamePtr(procCfgPath, "/");

            // Create the process.
            proc_Ref_t procPtr;
            if ((procPtr = proc_Create(procNamePtr, appPtr, procCfgPath)) == NULL)
            {
                goto failed;
            }

            // Add the process to the app's process list.
            ProcContainer_t* procContainerPtr = CreateProcContainer(appPtr, procPtr);

            le_dls_Queue(&(appPtr->procs), &(procContainerPtr->link));
        }
        while (le_cfg_GoToNextSibling(cfgIterator) == LE_OK);
    }

    // Set the resource limit for this application.
    if (resLim_SetAppLimits(appPtr) != LE_OK)
    {
        LE_ERROR("Could not set application resource limits.  Application %s cannot be started.",
                 appPtr->name);

        goto failed;
    }

    // Set SMACK rules for this app.
    // Setup the runtime area in the file system.
    if ( (SetSmackRules(appPtr) != LE_OK) ||
         (SetupAppArea(appPtr) != LE_OK) )
    {
        goto failed;
    }

    le_cfg_CancelTxn(cfgIterator);
    return appPtr;

failed:

    app_Delete(appPtr);
    le_cfg_CancelTxn(cfgIterator);
    return NULL;
}


//--------------------------------------------------------------------------------------------------
/**
 * Deletes all process containers from the specified list
 */
//--------------------------------------------------------------------------------------------------
static void DeleteProcContainersList
(
    le_dls_List_t procsList             ///< [IN] List of proc containers to delete.
)
{
    // Pop all the processes off the list and free them.
    le_dls_Link_t* procLinkPtr = le_dls_Pop(&procsList);

    while (procLinkPtr != NULL)
    {
        ProcContainer_t* procContainerPtr = CONTAINER_OF(procLinkPtr, ProcContainer_t, link);

        proc_Delete(procContainerPtr->procRef);
        le_mem_Release(procContainerPtr);

        procLinkPtr = le_dls_Pop(&procsList);
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Deletes an application.  The application must be stopped before it is deleted.
 *
 * @note If this function fails it will kill the calling process.
 */
//--------------------------------------------------------------------------------------------------
void app_Delete
(
    app_Ref_t appRef                    ///< [IN] Reference to the application to delete.
)
{
    CleanupAppSmackSettings(appRef);

    // Remove the resource limits.
    resLim_CleanupApp(appRef);

    // Delete all the process containers.
    DeleteProcContainersList(appRef->procs);
    DeleteProcContainersList(appRef->auxProcs);

    // Release the app timer.
    if (appRef->killTimer != NULL)
    {
        le_timer_Delete(appRef->killTimer);
    }

    // Relesase app.
    le_mem_Release(appRef);
}


//--------------------------------------------------------------------------------------------------
/**
 * Starts an application.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
le_result_t app_Start
(
    app_Ref_t appRef                    ///< [IN] Reference to the application to start.
)
{
    if (appRef->state == APP_STATE_RUNNING)
    {
        LE_ERROR("Application '%s' is already running.", appRef->name);

        return LE_FAULT;
    }

    appRef->state = APP_STATE_RUNNING;

    // Create /tmp for sandboxed apps and link in /tmp files.
    if (appRef->sandboxed)
    {
        // Get the SMACK label for the folders we create.
        char appDirLabel[LIMIT_MAX_SMACK_LABEL_BYTES];
        smack_GetAppAccessLabel(app_GetName(appRef), S_IRWXU, appDirLabel, sizeof(appDirLabel));

        // Create the app's /tmp for sandboxed apps.
        if (CreateTmpFs(appRef, appDirLabel) != LE_OK)
        {
            return LE_FAULT;
        }

        // Create default links.
        if (CreateDefaultTmpLinks(appRef, appDirLabel) != LE_OK)
        {
            return LE_FAULT;
        }
    }

    // Start all the processes in the application.
    le_dls_Link_t* procLinkPtr = le_dls_Peek(&(appRef->procs));

    while (procLinkPtr != NULL)
    {
        ProcContainer_t* procContainerPtr = CONTAINER_OF(procLinkPtr, ProcContainer_t, link);

        le_result_t result = proc_Start(procContainerPtr->procRef);

        if (result != LE_OK)
        {
            LE_ERROR("Could not start all application processes.  Stopping the application '%s'.",
                     appRef->name);

            app_Stop(appRef);

            return LE_FAULT;
        }

        // Get the next process.
        procLinkPtr = le_dls_PeekNext(&(appRef->procs), procLinkPtr);
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Stops an application.  This is an asynchronous function call that returns immediately but
 * the application may not stop right away.  Check the application's state with app_GetState() to
 * see when the application actually stops.
 */
//--------------------------------------------------------------------------------------------------
void app_Stop
(
    app_Ref_t appRef                    ///< [IN] Reference to the application to stop.
)
{
    LE_INFO("Stopping app '%s'", appRef->name);

    if (appRef->state == APP_STATE_STOPPED)
    {
        LE_ERROR("Application '%s' is already stopped.", appRef->name);
        return;
    }

    // Soft kill all the processes in the app.
    if (KillAppProcs(appRef, KILL_SOFT) == LE_NOT_FOUND)
    {
        // There are no more running processes in the app.
        LE_INFO("app '%s' has stopped.", appRef->name);

        appRef->state = APP_STATE_STOPPED;
    }
    else
    {
        // Start the kill timeout timer for this app.
        if (appRef->killTimer == NULL)
        {
            char timerName[LIMIT_MAX_PATH_BYTES];

            snprintf(timerName, sizeof(timerName), "%s_Killer", appRef->name);
            appRef->killTimer = le_timer_Create(timerName);

            LE_ASSERT(le_timer_SetInterval(appRef->killTimer, KillTimeout) == LE_OK);

            LE_ASSERT(le_timer_SetContextPtr(appRef->killTimer, (void*)appRef) == LE_OK);
            LE_ASSERT(le_timer_SetHandler(appRef->killTimer, HardKillApp) == LE_OK);
        }

        le_timer_Start(appRef->killTimer);
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets an application's state.
 *
 * @return
 *      The application's state.
 */
//--------------------------------------------------------------------------------------------------
app_State_t app_GetState
(
    app_Ref_t appRef                    ///< [IN] Reference to the application to stop.
)
{
    return appRef->state;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets the state of a process belonging to an application.
 *
 * @return
 *      The process's state.
 */
//--------------------------------------------------------------------------------------------------
app_ProcState_t app_GetProcState
(
    app_Ref_t appRef,                   ///< [IN] Reference to the application to stop.
    const char* procName                ///< [IN] Name of the process.
)
{
    if (appRef->state == APP_STATE_RUNNING)
    {
        // Find the process in the app's list.
        le_dls_Link_t* procLinkPtr = le_dls_Peek(&(appRef->procs));

        while (procLinkPtr != NULL)
        {
            ProcContainer_t* procContainerPtr = CONTAINER_OF(procLinkPtr, ProcContainer_t, link);

            if (strcmp(procName, proc_GetName(procContainerPtr->procRef)) == 0)
            {
                switch (proc_GetState(procContainerPtr->procRef))
                {
                    case PROC_STATE_STOPPED:
                        return APP_PROC_STATE_STOPPED;

                    case PROC_STATE_RUNNING:
                        return APP_PROC_STATE_RUNNING;

                    default:
                        LE_FATAL("Unrecognized process state.");
                }
            }

            procLinkPtr = le_dls_PeekNext(&(appRef->procs), procLinkPtr);
        }
    }

    return APP_PROC_STATE_STOPPED;
}


//--------------------------------------------------------------------------------------------------
/**
 * Checks if a given app is running a top-level process with given PID.
 *
 * An app's top-level processes are those that are started by the Supervisor directly.
 * If the Supervisor starts a process and that process starts another process, this function
 * will not find that second process.
 *
 * @return
 *      true if the process is one of this app's top-level processes, false if not.
 */
//--------------------------------------------------------------------------------------------------
bool app_HasTopLevelProc
(
    app_Ref_t appRef,
    pid_t pid
)
{
    return (FindProcContainer(appRef, pid) != NULL);
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets an application's name.
 *
 * @return
 *      The application's name.
 */
//--------------------------------------------------------------------------------------------------
const char* app_GetName
(
    app_Ref_t appRef                    ///< [IN] The application reference.
)
{
    return (const char*)appRef->name;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets an application's UID.
 *
 * @return
 *      The application's UID.
 */
//--------------------------------------------------------------------------------------------------
uid_t app_GetUid
(
    app_Ref_t appRef                    ///< [IN] The application reference.
)
{
    return appRef->uid;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets an application's GID.
 *
 * @return
 *      The application's GID.
 */
//--------------------------------------------------------------------------------------------------
gid_t app_GetGid
(
    app_Ref_t appRef                    ///< [IN] The application reference.
)
{
    return appRef->gid;
}


//--------------------------------------------------------------------------------------------------
/**
 * Check to see if the application is sandboxed or not.
 *
 * @return
 *      True if the app is sandboxed, false if not.
 */
//--------------------------------------------------------------------------------------------------
bool app_GetIsSandboxed
(
    app_Ref_t appRef                    ///< [IN] The application reference.
)
{
    return appRef->sandboxed;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets the directory path for an app's installation directory in the current running system.
 *
 * @return
 *      The absolute directory path.
 */
//--------------------------------------------------------------------------------------------------
const char* app_GetInstallDirPath
(
    app_Ref_t appRef                    ///< [IN] The application reference.
)
//--------------------------------------------------------------------------------------------------
{
    return (const char*)appRef->installDirPath;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets an application's working directory.
 *
 * @return
 *      The application's sandbox path.
 */
//--------------------------------------------------------------------------------------------------
const char* app_GetWorkingDir
(
    app_Ref_t appRef                    ///< [IN] The application reference.
)
{
    return (const char*)appRef->workingDir;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets an application's configuration path.
 *
 * @return
 *      The application's configuration path.
 */
//--------------------------------------------------------------------------------------------------
const char* app_GetConfigPath
(
    app_Ref_t appRef                    ///< [IN] The application reference.
)
{
    return (const char*)appRef->cfgPathRoot;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets an application's supplementary groups list.
 *
 * @return
 *      LE_OK if successful.
 *      LE_OVERFLOW if the buffer was too small to hold all the gids.
 */
//--------------------------------------------------------------------------------------------------
le_result_t app_GetSupplementaryGroups
(
    app_Ref_t appRef,                   ///< [IN] The application reference.
    gid_t* groupsPtr,                   ///< [OUT] List of group IDs.
    size_t* numGroupsPtr                ///< [IN/OUT] Size of groupsPtr buffer on input.  Number of
                                        ///           groups in groupsPtr on output.
)
{
    int i = 0;

    if (*numGroupsPtr >= appRef->numSupplementGids)
    {
        for (i = 0; i < appRef->numSupplementGids; i++)
        {
            groupsPtr[i] = appRef->supplementGids[i];
        }

        *numGroupsPtr = appRef->numSupplementGids;

        return LE_OK;
    }
    else
    {
        for (i = 0; i < *numGroupsPtr; i++)
        {
            groupsPtr[i] = appRef->supplementGids[i];
        }

        *numGroupsPtr = appRef->numSupplementGids;

        return LE_OVERFLOW;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * This handler must be called when the watchdog expires for a process that belongs to the
 * specified application.
 *
 * @return
 *      LE_OK if successful.
 *      LE_NOT_FOUND if the procPid was not found for the specified app.
 *
 *      The watchdog action passed in will be set to the action that should be taken for
 *      this process or one of the following:
 *          WATCHDOG_ACTION_NOT_FOUND - no action was configured for this process
 *          WATCHDOG_ACTION_ERROR     - the action could not be read or is unknown
 *          WATCHDOG_ACTION_HANDLED   - no further action is required, it is already handled.
 */
//--------------------------------------------------------------------------------------------------
le_result_t app_WatchdogTimeoutHandler
(
    app_Ref_t appRef,                                 ///< [IN] The application reference.
    pid_t procPid,                                    ///< [IN] The pid of the process that changed
                                                      ///< state.
    wdog_action_WatchdogAction_t* watchdogActionPtr   ///< [OUT] The fault action that should be
                                                      ///< taken.
)
{
    LE_FATAL_IF(appRef == NULL, "appRef is NULL");

    ProcContainer_t* procContainerPtr = FindProcContainer(appRef, procPid);

    if (procContainerPtr == NULL)
    {
        return LE_NOT_FOUND;
    }

    proc_Ref_t procRef = procContainerPtr->procRef;

    // Get the current process fault action.
    wdog_action_WatchdogAction_t watchdogAction = proc_GetWatchdogAction(procRef);

    // If WATCHDOG_ACTION_ERROR, we have reported the error already in proc. Let's give ourselves a
    // second chance and see if we can find a good value at app level.
    if (WATCHDOG_ACTION_NOT_FOUND == watchdogAction || WATCHDOG_ACTION_ERROR == watchdogAction)
    {
        // No action was defined for the proc. See if there is one for the app.
        // Read the app's watchdog action from the config tree.
        le_cfg_IteratorRef_t appCfg = le_cfg_CreateReadTxn(appRef->cfgPathRoot);

        char watchdogActionStr[LIMIT_MAX_FAULT_ACTION_NAME_BYTES];
        le_result_t result = le_cfg_GetString(appCfg, wdog_action_GetConfigNode(),
                watchdogActionStr, sizeof(watchdogActionStr), "");

        le_cfg_CancelTxn(appCfg);

        // Set the watchdog action based on the watchdog action string.
        if (result == LE_OK)
        {
            LE_DEBUG("%s watchdogAction '%s' in app section", appRef->name, watchdogActionStr);
            watchdogAction = wdog_action_EnumFromString(watchdogActionStr);
            if (WATCHDOG_ACTION_ERROR == watchdogAction)
            {
                LE_WARN("%s watchdog Action %s unknown", appRef->name, watchdogActionStr);
            }
        }
        else
        {
            LE_CRIT("Watchdog action string for application '%s' is too long.",
                    appRef->name);
            watchdogAction = WATCHDOG_ACTION_ERROR;
        }
    }

    // Set the action pointer to error. If it's still error when we leave here something
    // has gone wrong!!
    *watchdogActionPtr = WATCHDOG_ACTION_ERROR;

    // TODO: do watchdog timeouts count toward this total?
    switch (watchdogAction)
    {
        case WATCHDOG_ACTION_NOT_FOUND:
            LE_CRIT("The watchdog for process '%s' in app '%s' has timed out but there is no \
policy. The process will be restarted by default.", proc_GetName(procRef), appRef->name);

            // Set the process to restart when it stops then stop it.
            procContainerPtr->stopHandler = proc_Start;
            StopProc(procRef);
            *watchdogActionPtr = WATCHDOG_ACTION_HANDLED;
            break;

        case WATCHDOG_ACTION_IGNORE:
            LE_CRIT("The watchdog for process '%s' in app '%s' has timed out and will be ignored \
in accordance with its timeout policy.", proc_GetName(procRef), appRef->name);
            *watchdogActionPtr = WATCHDOG_ACTION_HANDLED;
            break;

        case WATCHDOG_ACTION_STOP:
            LE_CRIT("The watchdog for process '%s' in app '%s' has timed out and will be terminated \
in accordance with its timeout policy.", proc_GetName(procRef), appRef->name);
            StopProc(procRef);
            *watchdogActionPtr = WATCHDOG_ACTION_HANDLED;
            break;

        case WATCHDOG_ACTION_RESTART:
            LE_CRIT("The watchdog for process '%s' in app '%s' has timed out and will be restarted \
in accordance with its timeout policy.", proc_GetName(procRef), appRef->name);

            // Set the process to restart when it stops then stop it.
            procContainerPtr->stopHandler = proc_Start;
            StopProc(procRef);
            *watchdogActionPtr = WATCHDOG_ACTION_HANDLED;
            break;

        case WATCHDOG_ACTION_RESTART_APP:
            LE_CRIT("The watchdog for process '%s' in app '%s' has timed out and the app will be \
restarted in accordance with its timeout policy.", proc_GetName(procRef), appRef->name);

            *watchdogActionPtr = watchdogAction;
            break;

        case WATCHDOG_ACTION_STOP_APP:
            LE_CRIT("The watchdog for process '%s' in app '%s' has timed out and the app will \
be stopped in accordance with its timeout policy.", proc_GetName(procRef), appRef->name);

            *watchdogActionPtr = watchdogAction;
            break;

        case WATCHDOG_ACTION_REBOOT:
            LE_EMERG("The watchdog for process '%s' in app '%s' has timed out and the system will \
now be rebooted in accordance with its timeout policy.", proc_GetName(procRef), appRef->name);

            *watchdogActionPtr = watchdogAction;
            break;

        case WATCHDOG_ACTION_ERROR:
            // something went wrong reading the action.
            LE_CRIT("An error occurred trying to find the watchdog action for process '%s' in \
application '%s'. Restarting app by default.", proc_GetName(procRef), appRef->name);
            *watchdogActionPtr = WATCHDOG_ACTION_HANDLED;
            break;

        case WATCHDOG_ACTION_HANDLED:
            *watchdogActionPtr = watchdogAction;
            break;
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * This handler must be called when a SIGCHILD is received for a process that belongs to the
 * specified application.
 */
//--------------------------------------------------------------------------------------------------
void app_SigChildHandler
(
    app_Ref_t appRef,                   ///< [IN] The application reference.
    pid_t procPid,                      ///< [IN] The pid of the process that changed state.
    int procExitStatus,                 ///< [IN] The return status of the process given by wait().
    FaultAction_t* faultActionPtr       ///< [OUT] The fault action that should be taken.
)
{
    *faultActionPtr = FAULT_ACTION_IGNORE;

    ProcContainer_t* procContainerPtr = FindProcContainer(appRef, procPid);

    if (procContainerPtr != NULL)
    {
        // This proc has died call its external stop handler to inform interested parties of this
        // death.
        if (procContainerPtr->externStopHandler != NULL)
        {
            procContainerPtr->externStopHandler(procExitStatus, procContainerPtr->externContextPtr);
        }

        // Tell the "proc" module to handle the signal.  It will tell us what it wants us to do
        // about it, based on the process's faultAction.
        proc_Ref_t procRef = procContainerPtr->procRef;

        FaultAction_t procFaultAction = proc_SigChildHandler(procRef, procExitStatus);

        switch (procFaultAction)
        {
            case FAULT_ACTION_NONE:
                // This is something that happens if we have deliberately killed the proc or the
                // proc has terminated normally (EXIT_SUCCESS).
                // If the wdog stopped it then we may get here with an attached stop handler (to
                // call StartProc).
                if (procContainerPtr->stopHandler)
                {
                    if (procContainerPtr->stopHandler(procRef) != LE_OK)
                    {
                        LE_ERROR("Watchdog could not restart process '%s' in app '%s'.",
                                proc_GetName(procRef), appRef->name);

                        *faultActionPtr = FAULT_ACTION_STOP_APP;
                    }
                }
                break;

            case FAULT_ACTION_IGNORE:
                LE_WARN("Process '%s' in app '%s' faulted: Ignored.",
                        proc_GetName(procRef),
                        appRef->name);
                break;

            case FAULT_ACTION_RESTART_PROC:
                LE_CRIT("Process '%s' in app '%s' faulted: Restarting process.",
                        proc_GetName(procRef),
                        appRef->name);

                // Restart the process now.
                if (proc_Start(procRef) != LE_OK)
                {
                    LE_ERROR("Could not restart process '%s' in app '%s'.",
                             proc_GetName(procRef), appRef->name);

                    *faultActionPtr = FAULT_ACTION_STOP_APP;
                }
                break;

            case FAULT_ACTION_RESTART_APP:
                LE_CRIT("Process '%s' in app '%s' faulted: Restarting app.",
                        proc_GetName(procRef),
                        appRef->name);

                *faultActionPtr = FAULT_ACTION_RESTART_APP;
                break;

            case FAULT_ACTION_STOP_APP:
                LE_CRIT("Process '%s' in app '%s' faulted: Stopping app.",
                        proc_GetName(procRef),
                        appRef->name);

                *faultActionPtr = FAULT_ACTION_STOP_APP;
                break;

            case FAULT_ACTION_REBOOT:
                LE_EMERG("Process '%s' in app '%s' faulted: Rebooting system.",
                         proc_GetName(procRef),
                         appRef->name);

                *faultActionPtr = FAULT_ACTION_REBOOT;
                break;
        }
    }

    // If all the processes in the app have now died,
    if (!HasRunningProc(appRef))
    {
        // If we've been trying to kill this thing, then we can stop the time-out timer now.
        if (appRef->killTimer != NULL)
        {
            le_timer_Stop(appRef->killTimer);
        }

        LE_DEBUG("app '%s' has stopped.", appRef->name);

        appRef->state = APP_STATE_STOPPED;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Creates a reference to an application process.
 *
 * If the process name refers to an existing configured application process then a reference to that
 * process is simply returned.  In this case an executable path may be specified to override the
 * configured executable.
 *
 * If the process name does not match any configured application processes then a new process
 * is created.  In this case an executable path must be specified.
 *
 * Configured processes take their runtime parameters, such as environment variables, priority, etc.
 * from the configuration database while non-configured processes use default parameters.
 *
 * Parameters can be overridden by the other functions in this API such as app_AddProcArg(),
 * app_SetProcPriority(), etc.
 *
 * It is an error to call this function on a configured process that is already running.
 *
 * @return
 *      Reference to the application process if successful.
 *      NULL if there was an error.
 */
//--------------------------------------------------------------------------------------------------
app_Proc_Ref_t app_CreateProc
(
    app_Ref_t appRef,                   ///< [IN] The application reference.
    const char* procNamePtr,            ///< [IN] Name of the process.
    const char* execPathPtr             ///< [IN] Path to the executable file.
)
{
    // See if the process already exists.
    ProcContainer_t* procContainerPtr = GetProcContainer(appRef, procNamePtr);

    if (procContainerPtr == NULL)
    {
        // This is not a configured process so make sure the executable path is provided.
        if (execPathPtr == NULL)
        {
            LE_ERROR("Executable path for process %s in app %s cannot be empty.",
                     procNamePtr, appRef->name);

            return NULL;
        }

        // If the process name is empty use the base name of the executable as the process name.
        const char* procNameToUsePtr = procNamePtr;

        if (procNameToUsePtr == NULL)
        {
            procNameToUsePtr = le_path_GetBasenamePtr(execPathPtr, "/");
        }

        // Create the process.
        proc_Ref_t procPtr;
        if ((procPtr = proc_Create(procNameToUsePtr, appRef, NULL)) == NULL)
        {
            return NULL;
        }

        // Store the executable path.
        if (proc_SetExecPath(procPtr, execPathPtr) != LE_OK)
        {
            LE_ERROR("Executable path '%s' is too long.", execPathPtr);

            proc_Delete(procPtr);
            return NULL;
        }

        // Create the process container.
        procContainerPtr = CreateProcContainer(appRef, procPtr);

        // Add the process to the app's auxiliary process list.
        le_dls_Queue(&(appRef->auxProcs), &(procContainerPtr->link));
    }
    else
    {
        // This is a configured process.
        if (proc_GetState(procContainerPtr->procRef) == PROC_STATE_RUNNING)
        {
            LE_ERROR("Process '%s' in app '%s' is already running.", procNamePtr, appRef->name);
            return NULL;
        }

        if (execPathPtr != NULL)
        {
             // Set the executable path.
            if (proc_SetExecPath(procContainerPtr->procRef, execPathPtr) != LE_OK)
            {
                LE_ERROR("Executable path '%s' is too long.", execPathPtr);
                return NULL;
            }
        }
    }

    return procContainerPtr;
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets the standard in of an application process.
 */
//--------------------------------------------------------------------------------------------------
void app_SetProcStdIn
(
    app_Proc_Ref_t appProcRef,  ///< [IN] Process reference.
    int stdInFd                 ///< [IN] File descriptor to use as the app proc's standard in.
)
{
    proc_SetStdIn(appProcRef->procRef, stdInFd);
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets the standard out of an application process.
 */
//--------------------------------------------------------------------------------------------------
void app_SetProcStdOut
(
    app_Proc_Ref_t appProcRef,  ///< [IN] Process reference.
    int stdOutFd                ///< [IN] File descriptor to use as the app proc's standard out.
)
{
    proc_SetStdOut(appProcRef->procRef, stdOutFd);
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets the standard error of an application process.
 */
//--------------------------------------------------------------------------------------------------
void app_SetProcStdErr
(
    app_Proc_Ref_t appProcRef,  ///< [IN] Process reference.
    int stdErrFd                ///< [IN] File descriptor to use as the app proc's standard error.
)
{
    proc_SetStdErr(appProcRef->procRef, stdErrFd);
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets a stop handler to be called when the specified process stops.
 */
//--------------------------------------------------------------------------------------------------
void app_SetProcStopHandler
(
    app_Proc_Ref_t appProcRef,                  ///< [IN] Process reference.
    app_Proc_StopHandlerFunc_t stopHandler,     ///< [IN] Handler to call when the process exits.
    void* stopHandlerContextPtr                 ///< [IN] Context data for the stop handler.
)
{
    appProcRef->externStopHandler = stopHandler;
    appProcRef->externContextPtr = stopHandlerContextPtr;
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets the process's priority.
 *
 * This overrides the configured priority if available.
 *
 * The priority level string can be either "idle", "low", "medium", "high", "rt1" ... "rt32".
 *
 * @return
 *      LE_OK if successful.
 *      LE_OVERFLOW if the priority string is too long.
 *      LE_FAULT if the priority string is not valid.
 */
//--------------------------------------------------------------------------------------------------
le_result_t app_SetProcPriority
(
    app_Proc_Ref_t appProcRef,  ///< [IN] Process reference.
    const char* priorityPtr     ///< [IN] Priority string.  NULL to clear the priority.
)
{
    return proc_SetPriority(appProcRef->procRef, priorityPtr);
}


//--------------------------------------------------------------------------------------------------
/**
 * Adds a cmd-line argument to a process.  Adding a NULL argPtr is valid and can be used to validate
 * the args list without actually adding an argument.  This is useful for overriding the configured
 * arguments with an empty list.
 *
 * This overrides the configured arguments if available.
 *
 * @return
 *      LE_OK if successful.
 *      LE_OVERFLOW if the argument string is too long.
 */
//--------------------------------------------------------------------------------------------------
le_result_t app_AddArgs
(
    app_Proc_Ref_t appProcRef,  ///< [IN] Process reference.
    const char* argPtr          ///< [IN] Argument string.
)
{
    return proc_AddArgs(appProcRef->procRef, argPtr);
}


//--------------------------------------------------------------------------------------------------
/**
 * Deletes and invalidates the cmd-line arguments to a process.  This means the process will only
 * use arguments from the config if available.
 */
//--------------------------------------------------------------------------------------------------
void app_ClearArgs
(
    app_Proc_Ref_t appProcRef   ///< [IN] Process reference.
)
{
    proc_ClearArgs(appProcRef->procRef);
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets fault action for a process.
 *
 * This overrides the configured fault action if available.
 *
 * The fault action can be set to FAULT_ACTION_NONE to indicate that the configured fault action
 * should be used if available.
 */
//--------------------------------------------------------------------------------------------------
void app_SetFaultAction
(
    app_Proc_Ref_t appProcRef,          ///< [IN] Process reference.
    FaultAction_t faultAction           ///< [IN] Fault action.
)
{
    proc_SetFaultAction(appProcRef->procRef, faultAction);
}


//--------------------------------------------------------------------------------------------------
/**
 * Starts an application process.  This function assumes that the app has already started.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was some other error.
 */
//--------------------------------------------------------------------------------------------------
le_result_t app_StartProc
(
    app_Proc_Ref_t appProcRef                   ///< [IN] Process reference.
)
{
    if (proc_GetState(appProcRef->procRef) == PROC_STATE_STOPPED)
    {
        return proc_Start(appProcRef->procRef);
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Deletes an application process from an app.
 *
 * If the process is running stop it first.
 *
 * If the process is a configured process the overriden parameters are cleared but the process is
 * not actually deleted.
 */
//--------------------------------------------------------------------------------------------------
void app_DeleteProc
(
    app_Ref_t appRef,                           ///< [IN] App reference.
    app_Proc_Ref_t appProcRef                   ///< [IN] Process reference.
)
{
    if (proc_GetState(appProcRef->procRef) == PROC_STATE_RUNNING)
    {
        StopProc(appProcRef->procRef);
    }

    if (proc_GetConfigPath(appProcRef->procRef) != NULL)
    {
        // This is a configured process.  Simply reset all of the overrides.
        proc_SetStdIn(appProcRef->procRef, -1);
        proc_SetStdOut(appProcRef->procRef, -1);
        proc_SetStdErr(appProcRef->procRef, -1);

        proc_SetExecPath(appProcRef->procRef, NULL);
        proc_SetPriority(appProcRef->procRef, NULL);
        proc_ClearArgs(appProcRef->procRef);
        proc_SetFaultAction(appProcRef->procRef, FAULT_ACTION_NONE);

        appProcRef->externStopHandler = NULL;
        appProcRef->externContextPtr = NULL;
    }
    else
    {
        // This is an auxiliary process.  Delete it.
        le_dls_Remove(&(appRef->auxProcs), &(appProcRef->link));

        proc_Delete(appProcRef->procRef);
        le_mem_Release(appProcRef);
    }
}
