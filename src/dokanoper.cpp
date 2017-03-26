#include<ntstatus.h>

#include<stdio.h>
#include<stdio.h>
#include<string.h>
#include<wchar.h>
#include<errno.h>
#include<assert.h>
#include<map>
#include"CSimpleFS.h"
#include"CDirectory.h"

#include"dokanoper.h"

extern "C" {
    #include <dokan/dokan.h>
}
// fix some bad defines
#undef CreateDirectory
#undef CreateFile

static SimpleFilesystem *fs;
static uint64_t handleid = 1;

static std::map<ULONG64, std::string> handle2path;

// -----------------------------------------------

#include <boost/locale/encoding_utf.hpp>
using boost::locale::conv::utf_to_utf;

std::wstring utf8_to_wstring(const std::string& str)
{
    return utf_to_utf<wchar_t>(str.c_str(), str.c_str() + str.size());
}

std::string wstring_to_utf8(const std::wstring& str)
{
    return utf_to_utf<char>(str.c_str(), str.c_str() + str.size());
}

std::string GetFilePath(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo)
{
    if (DokanFileInfo->Context == 0)
    {
        return wstring_to_utf8(FileName);
    } else
    {
        return handle2path[DokanFileInfo->Context];
    }
}

// -----------------------------------------------

NTSTATUS errno_to_nstatus(int err)
{
    if (err < 0) err = -err;
    switch(err)
    {
    case ENOENT:
        return STATUS_OBJECT_NAME_NOT_FOUND;
    case EEXIST:
        return STATUS_OBJECT_NAME_COLLISION;
    case ENOMEM:
        return STATUS_NO_MEMORY;
    case ENOTEMPTY:
        return STATUS_DIRECTORY_NOT_EMPTY;
    case ENOTDIR:
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    return ERROR_INVALID_FUNCTION;
}
// -----------------------------------------------

static NTSTATUS DOKAN_CALLBACK
Dokan_CreateFile(
    LPCWSTR FileName,
    PDOKAN_IO_SECURITY_CONTEXT SecurityContext,
    ACCESS_MASK DesiredAccess,
    ULONG FileAttributes,
    ULONG ShareAccess,
    ULONG CreateDisposition,
    ULONG CreateOptions,
    PDOKAN_FILE_INFO DokanFileInfo)
{
    std::string path = wstring_to_utf8(FileName);

    printf("\nCreateFile '%s' (new handle id %lu) ", path.c_str(), handleid);

    if (path == "") return STATUS_OBJECT_NAME_NOT_FOUND;

    if (CreateOptions&FILE_DIRECTORY_FILE) printf(" (isdir) ");

    // Specifies the action to perform if the file does or does not exist.
    switch(CreateDisposition)
    {
    case CREATE_NEW:
        printf("(create new)");
        break;

    case OPEN_ALWAYS:
        printf("(open always)");
        break;

    case CREATE_ALWAYS:
        printf("(create always)");
        break;

    case OPEN_EXISTING:
        printf("(open existing)");
        break;

    case TRUNCATE_EXISTING:
        printf("(Truncate existing)");
        break;

    default:
        printf("(unknown)");
        break;
    }

    if ((DesiredAccess&GENERIC_READ)) printf("(generic read)");
    if ((DesiredAccess&GENERIC_WRITE)) printf("(generic write)");
    if ((DesiredAccess&GENERIC_EXECUTE)) printf("(generic execute)");

    if ((DesiredAccess&DELETE)) printf("(delete)");
    if ((DesiredAccess&FILE_READ_DATA)) printf("(read data)");
    if ((DesiredAccess&FILE_READ_ATTRIBUTES)) printf("(read attributes)");
    if ((DesiredAccess&FILE_READ_EA)) printf("(read ea)");
    //if ((DesiredAccess&FILE_READ_CONTROL)) printf("(read control)");
    if ((DesiredAccess&FILE_WRITE_DATA)) printf("(write data)");
    if ((DesiredAccess&FILE_WRITE_ATTRIBUTES)) printf("(write attributes)");
    if ((DesiredAccess&FILE_WRITE_EA)) printf("(write ea)");
    if ((DesiredAccess&FILE_APPEND_DATA)) printf("(append data)");
    //if ((DesiredAccess&FILE_WRITE_DAC)) printf("(write dac)");
    //if ((DesiredAccess&FILE_WRITE_OWNER)) printf("(write owner)");
    if ((DesiredAccess&SYNCHRONIZE)) printf("(synchronize)");
    if ((DesiredAccess&FILE_EXECUTE)) printf("(file execute)");
    if ((DesiredAccess&STANDARD_RIGHTS_READ)) printf("(standard rights read)");
    if ((DesiredAccess&STANDARD_RIGHTS_WRITE)) printf("(standard rights write)");
    if ((DesiredAccess&STANDARD_RIGHTS_EXECUTE)) printf("(standard rights execute)");
    if ((DesiredAccess&FILE_LIST_DIRECTORY)) printf("(file list directory)");
    if ((DesiredAccess&FILE_TRAVERSE)) printf("(file traverse)");
    printf("\n");
    
    if ((CreateOptions & FILE_DIRECTORY_FILE) == FILE_DIRECTORY_FILE)
    {
        if (CreateDisposition == FILE_CREATE || CreateDisposition == FILE_OPEN_IF)
        {
            //printf("command: create directory\n");

            std::vector<std::string> splitpath;
            splitpath = SplitPath(path);

            if (splitpath.size() == 0)
            {
                SetLastError(ERROR_ALREADY_EXISTS);
                DokanFileInfo->IsDirectory = TRUE;
                DokanFileInfo->Context = handleid++;
                handle2path[DokanFileInfo->Context] = path;
                return STATUS_SUCCESS;
            }
            //assert(splitpath.size() >= 1);

            std::string filename = splitpath.back();
            splitpath.pop_back();
            try
            {
                CDirectory dir = fs->OpenDir(splitpath);
                dir.CreateDirectory(filename);
                DokanFileInfo->Context = handleid++;
                DokanFileInfo->IsDirectory = TRUE;
                handle2path[DokanFileInfo->Context] = path;
            } catch(const int &err) // or file already exist?
            {
                return errno_to_nstatus(err);
            }
            return STATUS_SUCCESS;
        } else
        if (CreateDisposition == FILE_OPEN)
        {
            DokanFileInfo->IsDirectory = TRUE;

            //printf("command: open directory\n");
            try
            {
                INODEPTR node = fs->OpenNode(path.c_str());
                DokanFileInfo->Context = handleid++;
                handle2path[DokanFileInfo->Context] = path;
            } catch(const int &err)
            {
                return errno_to_nstatus(err);
            }
            return STATUS_SUCCESS;
        }
    }

    if (CreateDisposition == FILE_CREATE || CreateDisposition == FILE_OPEN_IF)
    {
        //printf("command: create file\n");

        std::vector<std::string> splitpath;
        splitpath = SplitPath(path);
        assert(splitpath.size() >= 1);
        std::string filename = splitpath.back();
        splitpath.pop_back();
        try
        {
            CDirectory dir = fs->OpenDir(splitpath);
            dir.CreateFile(filename);
            DokanFileInfo->Context = handleid++;
            handle2path[DokanFileInfo->Context] = path;
        } catch(const int &err) // or file already exist?
        {
            return errno_to_nstatus(err);
        }

        //SetLastError(ERROR_ALREADY_EXISTS);
        return STATUS_SUCCESS;
        //return errno_to_ntstatus_error(impl->create_directory(FileName, DokanFileInfo));
    } else
    if (CreateDisposition == FILE_OPEN)
    {
        //printf("command: open file\n");
        try
        {
            INODEPTR node = fs->OpenNode(path.c_str());
            DokanFileInfo->Context = handleid++;;
            handle2path[DokanFileInfo->Context] = path;
        } catch(const int &err)
        {
            return errno_to_nstatus(err);
        }
        return STATUS_SUCCESS;
    }

    return STATUS_SUCCESS;
}

// -----------------------------------------------

static NTSTATUS DOKAN_CALLBACK Dokan_GetFileInformation(LPCWSTR FileName, LPBY_HANDLE_FILE_INFORMATION HandleFileInformation, PDOKAN_FILE_INFO DokanFileInfo)
{
    std::string path = GetFilePath(FileName, DokanFileInfo);
    printf("GetFileInformation '%s' of handle %lli\n", path.c_str(), DokanFileInfo->Context);

    HANDLE handle = (HANDLE)DokanFileInfo->Context;
    if (!handle || handle == INVALID_HANDLE_VALUE)
    {
        printf("Invalid handle?\n");
    }

    //memset(HandleFileInformation, 0, sizeof(struct BY_HANDLE_FILE_INFORMATION));
    try
    {
        INODEPTR node = fs->OpenNode(path.c_str());
        //printf("open succesful\n");
        node->Lock();
        HandleFileInformation->nFileSizeLow = node->size&0xFFFFFFFF;
        HandleFileInformation->nFileSizeHigh = node->size >> 32;
        HandleFileInformation->nNumberOfLinks = 1;
        //HandleFileInformation->ftCreationTime = 0;
        //HandleFileInformation->ftLastAccessTime = 0;
        //HandleFileInformation->ftLastWriteTime = 0;
        HandleFileInformation->nFileIndexHigh = 0;
        HandleFileInformation->nFileIndexLow = node->id;
        HandleFileInformation->dwVolumeSerialNumber = 0x53281900;

        // FILE_ATTRIBUTE_OFFLINE
        if (node->type == INODETYPE::dir)
        {
            HandleFileInformation->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
            DokanFileInfo->IsDirectory = TRUE;
        } else
        {
            HandleFileInformation->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        }
        node->Unlock();
    } catch(const int &err)
    {
        return errno_to_nstatus(err);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK Dokan_SetFileAttributes(
LPCWSTR FileName, DWORD FileAttributes, PDOKAN_FILE_INFO DokanFileInfo)
{
    std::string path = GetFilePath(FileName, DokanFileInfo);
    printf("SetFileAttributes '%s' FileAttributes: 0x%08x\n", path.c_str(), FileAttributes);
    return STATUS_SUCCESS;
}
/*
static NTSTATUS DOKAN_CALLBACK Dokan_GetFileSecurity(
    LPCWSTR FileName, PSECURITY_INFORMATION SecurityInformation,
    PSECURITY_DESCRIPTOR SecurityDescriptor, ULONG BufferLength,
    PULONG LengthNeeded, PDOKAN_FILE_INFO DokanFileInfo)
{
    std::string path = GetFilePath(FileName, DokanFileInfo);
    printf("GetFileSecurity '%s' requested information: 0x%08x\n", path.c_str(), *SecurityInformation);


    if ((*SecurityInformation) & OWNER_SECURITY_INFORMATION)
    {
	printf("request owner security information\n");
    }
    if ((*SecurityInformation) & GROUP_SECURITY_INFORMATION)
    {
	printf("request group security information\n");
    }
    if ((*SecurityInformation) & DACL_SECURITY_INFORMATION)
    {
	printf("request dacl security information\n");
    }
    if ((*SecurityInformation) > 7)
    {
        return ERROR_INVALID_FUNCTION;
    }
    return ERROR_INVALID_FUNCTION;
    //return STATUS_SUCCESS;
}
*/
/*
NTSTATUS DOKAN_CALLBACK
Dokan_FindStreams(LPCWSTR FileName, PFillFindStreamData FillFindStreamData,
                PDOKAN_FILE_INFO DokanFileInfo)
{
    std::string path = wstring_to_utf8(FileName);
    printf("FindStreams '%s'\n", path.c_str());
    return STATUS_SUCCESS;
}
*/

// -----------------------------------------------

static NTSTATUS DOKAN_CALLBACK Dokan_SetEndOfFile(
    LPCWSTR FileName, LONGLONG ByteOffset, PDOKAN_FILE_INFO DokanFileInfo)
{
    std::string path = GetFilePath(FileName, DokanFileInfo);
    printf("SetEndOfFile '%s' size=%lli\n", path.c_str(), ByteOffset);

    try
    {
        INODEPTR node = fs->OpenFile(path);
        node->Truncate(ByteOffset, false);  // the content is undefined according to spec
        //node->Truncate(ByteOffset);
    } catch(const int &err)
    {
        return errno_to_nstatus(err);
    }

    return STATUS_SUCCESS;
}

// -----------------------------------------------

static NTSTATUS DOKAN_CALLBACK Dokan_ReadFile(LPCWSTR FileName, LPVOID Buffer,
DWORD BufferLength,
LPDWORD ReadLength,
LONGLONG Offset,
PDOKAN_FILE_INFO DokanFileInfo)
{
    std::string path = GetFilePath(FileName, DokanFileInfo);
    printf("ReadFile '%s' size=%i\n", path.c_str(), BufferLength);
    try
    {
        INODEPTR node = fs->OpenFile(path.c_str());
        *ReadLength = node->Read((int8_t*)Buffer, Offset, BufferLength);
    } catch(const int &err)
    {
        return errno_to_nstatus(err);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK Dokan_WriteFile(LPCWSTR FileName, LPCVOID Buffer,
DWORD NumberOfBytesToWrite,
LPDWORD NumberOfBytesWritten,
LONGLONG Offset,
PDOKAN_FILE_INFO DokanFileInfo)
{
    std::string path = GetFilePath(FileName, DokanFileInfo);
    printf("WriteFile '%s' size=%i\n", path.c_str(), NumberOfBytesToWrite);
    try
    {
        INODEPTR node = fs->OpenFile(path.c_str());
        node->Write((int8_t*)Buffer, Offset, NumberOfBytesToWrite);
        *NumberOfBytesWritten = NumberOfBytesToWrite;
    } catch(const int &err)
    {
        return errno_to_nstatus(err);
    }
    return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK
Dokan_FindFiles(LPCWSTR FileName, PFillFindData FillFindData, PDOKAN_FILE_INFO DokanFileInfo)
{
    std::string path = GetFilePath(FileName, DokanFileInfo);
    printf("FindFiles '%s'\n", path.c_str());

    try
    {
        CDirectory dir = fs->OpenDir(path.c_str());

        dir.ForEachEntry([&](DIRENTRY &de)
        {
            if ((INODETYPE)de.type == INODETYPE::free) return FOREACHENTRYRET::OK;
            WIN32_FIND_DATAW findData = {0};

            INODEPTR node = fs->OpenNode(de.id);
            node->Lock();
            findData.nFileSizeHigh = node->size >> 32;
            findData.nFileSizeLow = node->size & 0xFFFFFFFF;
            if ((INODETYPE)de.type == INODETYPE::dir)
            {
                findData.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
            } else
            {
                findData.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
            }
            node->Unlock();
            std::wstring name = utf8_to_wstring(de.name);
            wcsncpy(findData.cFileName, name.data(), 96+32);
            FillFindData(&findData, DokanFileInfo);

            return FOREACHENTRYRET::OK;
        });

    } catch(const int &err)
    {
        return errno_to_nstatus(err);
    }

    return STATUS_SUCCESS;
}

// -----------------------------------------------

static NTSTATUS DOKAN_CALLBACK
Dokan_MoveFile(LPCWSTR FileName, // existing file name
LPCWSTR NewFileName, BOOL ReplaceIfExisting, PDOKAN_FILE_INFO DokanFileInfo)
{
    std::string oldpath = GetFilePath(FileName, DokanFileInfo);
    std::string newpath = wstring_to_utf8(NewFileName);
    printf("MoveFile '%s' to '%s'\n", oldpath.c_str(), newpath.c_str());

    std::vector<std::string> splitpath;
    splitpath = SplitPath(newpath);
    assert(splitpath.size() >= 1);

    try
    {
        INODEPTR newnode = fs->OpenNode(splitpath);
        return errno_to_nstatus(-EEXIST);
    }
    catch(...){}

    try
    {
        INODEPTR node = fs->OpenNode(oldpath);

        std::string filename = splitpath.back();
        splitpath.pop_back();
        CDirectory dir = fs->OpenDir(splitpath);
        fs->Rename(node, dir, filename);
    } catch(const int &err)
    {
        return errno_to_nstatus(err);
    }

    /*
    WCHAR filePath[MAX_PATH];
    WCHAR newFilePath[MAX_PATH];
    BOOL status;

    GetFilePath(filePath, MAX_PATH, FileName);
    GetFilePath(newFilePath, MAX_PATH, NewFileName);

    DbgPrint(L"MoveFile %s -> %s\n\n", filePath, newFilePath);

    if (DokanFileInfo->Context) {
        // should close? or rename at closing?
        CloseHandle((HANDLE)DokanFileInfo->Context);
        DokanFileInfo->Context = 0;
    }

    if (ReplaceIfExisting)
    status = MoveFileEx(filePath, newFilePath, MOVEFILE_REPLACE_EXISTING);
    else
    status = MoveFile(filePath, newFilePath);

    if (status == FALSE) {
        DWORD error = GetLastError();
        DbgPrint(L"\tMoveFile failed status = %d, code = %d\n", status, error);
        return ToNtStatus(error);
    } else {
        return STATUS_SUCCESS;
    }
    */
    return STATUS_SUCCESS;
}

// -----------------------------------------------

static void DOKAN_CALLBACK Dokan_CloseFile(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo)
{
    std::string path = GetFilePath(FileName, DokanFileInfo);
    printf("CloseFile '%s' of handle %lli\n", path.c_str(), DokanFileInfo->Context);
    if (DokanFileInfo)
    if (DokanFileInfo->Context)
    {
        DokanFileInfo->Context = 0;
    }
}

static void DOKAN_CALLBACK Dokan_Cleanup(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo)
{
    std::string path = GetFilePath(FileName, DokanFileInfo);
    printf("Cleanup '%s' of handle %lli\n", path.c_str(), DokanFileInfo->Context);

    if (!DokanFileInfo->Context) return;

    if (!DokanFileInfo->DeleteOnClose) return;
    printf("remove file\n");
    try
    {
        INODEPTR node = fs->OpenNode(path);
        node->Remove();
    } catch(const int &err)
    {
        printf("Cannot remove file\n");
        return;
    }
}

// -----------------------------------------------

static NTSTATUS DOKAN_CALLBACK
Dokan_DeleteFile(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) 
{
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
Dokan_DeleteDirectory(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) 
{
    return STATUS_SUCCESS;
}

// -----------------------------------------------

static NTSTATUS DOKAN_CALLBACK Dokan_GetVolumeInformation(
LPWSTR VolumeNameBuffer, DWORD VolumeNameSize, LPDWORD VolumeSerialNumber,
LPDWORD MaximumComponentLength, LPDWORD FileSystemFlags,
LPWSTR FileSystemNameBuffer, DWORD FileSystemNameSize,
PDOKAN_FILE_INFO DokanFileInfo)
{
    UNREFERENCED_PARAMETER(DokanFileInfo);

    printf("GetVolumeInformation\n");
    wcsncpy(VolumeNameBuffer, L"CoverFS", VolumeNameSize);

    *VolumeSerialNumber = 0x53281900;
    *MaximumComponentLength = 255;

    *FileSystemFlags = FILE_CASE_SENSITIVE_SEARCH | FILE_CASE_PRESERVED_NAMES |
    FILE_SUPPORTS_REMOTE_STORAGE | FILE_UNICODE_ON_DISK  |
    FILE_PERSISTENT_ACLS;

    //*FileSystemFlags = 3;
    wcsncpy(FileSystemNameBuffer, L"Dokan", FileSystemNameSize);

    return STATUS_SUCCESS;
}
/*
static NTSTATUS DOKAN_CALLBACK Dokan_LockFile(LPCWSTR FileName,
                                            LONGLONG ByteOffset,
                                            LONGLONG Length,
                                            PDOKAN_FILE_INFO DokanFileInfo)
{
    printf("LockFile '%s'\n", wstring_to_utf8(FileName).c_str());
    return STATUS_SUCCESS;
}
*/
// -----------------------------------------------

static NTSTATUS DOKAN_CALLBACK Dokan_Mounted(PDOKAN_FILE_INFO DokanFileInfo)
{
    UNREFERENCED_PARAMETER(DokanFileInfo);

    printf("Mounted\n");
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK Dokan_Unmounted(PDOKAN_FILE_INFO DokanFileInfo)
{
    UNREFERENCED_PARAMETER(DokanFileInfo);

    printf("Unmounted\n");
    return STATUS_SUCCESS;
}

// -----------------------------------------------

int StartDokan(int argc, char *argv[], const char* mountpoint, SimpleFilesystem &_fs)
{
    fs = &_fs;

    DOKAN_OPERATIONS dokanOperations = {0};
    DOKAN_OPTIONS dokanOptions = {0};

    dokanOptions.Version = DOKAN_VERSION;
    dokanOptions.ThreadCount = 1; // use default = 0 is default
    //dokanOptions.Options |= DOKAN_OPTION_DEBUG;
    //dokanOptions.Options |= DOKAN_OPTION_STDERR;
    //dokanOptions.Options |= DOKAN_OPTION_ALT_STREAM;

    //dokanOptions.Options |= DOKAN_OPTION_NETWORK;
    dokanOptions.Options |= DOKAN_OPTION_REMOVABLE; // we have our own write cache
    //dokanOptions.Options |= DOKAN_OPTION_WRITE_PROTECT;
    //dokanOptions.Options |= DOKAN_OPTION_MOUNT_MANAGER;

    //LPCWSTR
    auto mp = utf8_to_wstring(mountpoint);
    dokanOptions.MountPoint = mp.data();

    dokanOperations.ZwCreateFile         = Dokan_CreateFile;
    dokanOperations.Cleanup              = Dokan_Cleanup;
    dokanOperations.CloseFile            = Dokan_CloseFile;
    dokanOperations.ReadFile             = Dokan_ReadFile;
    dokanOperations.WriteFile            = Dokan_WriteFile;
    dokanOperations.GetFileInformation   = Dokan_GetFileInformation;
    dokanOperations.FindFiles            = Dokan_FindFiles;
    dokanOperations.FindFilesWithPattern = NULL;
    dokanOperations.SetFileAttributes    = Dokan_SetFileAttributes;;
    dokanOperations.DeleteFile           = Dokan_DeleteFile;
    dokanOperations.DeleteDirectory      = Dokan_DeleteDirectory;

    dokanOperations.SetEndOfFile         = Dokan_SetEndOfFile;
    dokanOperations.Mounted              = Dokan_Mounted;
    dokanOperations.Unmounted            = Dokan_Unmounted;
    dokanOperations.GetVolumeInformation = Dokan_GetVolumeInformation;
    dokanOperations.MoveFile             = Dokan_MoveFile;
    dokanOperations.GetDiskFreeSpace     = NULL;
    //dokanOperations.FindStreams        = Dokan_FindStreams;
    //dokanOperations.GetFileSecurity      = Dokan_GetFileSecurity;
    //dokanOperations.LockFile           = Dokan_LockFile;
    dokanOperations.FindFilesWithPattern = NULL;


    int status = DokanMain(&dokanOptions, &dokanOperations);
    switch (status)
    {
    case DOKAN_SUCCESS:
        fprintf(stderr, "Success\n");
        break;
    case DOKAN_ERROR:
        fprintf(stderr, "Error\n");
        break;
    case DOKAN_DRIVE_LETTER_ERROR:
        fprintf(stderr, "Bad Drive letter\n");
        break;
    case DOKAN_DRIVER_INSTALL_ERROR:
        fprintf(stderr, "Can't install driver\n");
        break;
    case DOKAN_START_ERROR:
        fprintf(stderr, "Driver something wrong\n");
        break;
    case DOKAN_MOUNT_ERROR:
        fprintf(stderr, "Can't assign a drive letter\n");
        break;
    case DOKAN_MOUNT_POINT_ERROR:
        fprintf(stderr, "Mount point error\n");
        break;
    case DOKAN_VERSION_ERROR:
        fprintf(stderr, "Version error\n");
        break;
    default:
        fprintf(stderr, "Unknown error: %d\n", status);
        break;
    }

    return EXIT_SUCCESS;
}
