/* fhandler_socket_unix.cc.

   See fhandler.h for a description of the fhandler classes.

   This file is part of Cygwin.

   This software is a copyrighted work licensed under the terms of the
   Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
   details. */

#include "winsup.h"
#include <w32api/winioctl.h>
#include <asm/byteorder.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/param.h>
#include <sys/statvfs.h>
#include <cygwin/acl.h>
#include "cygerrno.h"
#include "path.h"
#include "fhandler.h"
#include "dtable.h"
#include "cygheap.h"
#include "hires.h"
#include "shared_info.h"
#include "ntdll.h"
#include "miscfuncs.h"
#include "tls_pbuf.h"

/*
   Abstract socket:

     An abstract socket is represented by a symlink in the native
     NT namespace, within the Cygin subdir in BasedNamedObjects.
     So it's globally available but only exists as long as at least on
     descriptor on the socket is open, as desired.

     The name of the symlink is: "af-unix-<sun_path>"

     <sun_path> is the transposed sun_path string, including the leading
     NUL.  The transposition is simplified in that it uses every byte
     in the valid sun_path name as is, no extra multibyte conversion.
     The content of the symlink is the basename of the underlying pipe.

  Named socket:

    A named socket is represented by a reparse point with a Cygwin-specific
    tag and GUID.  The GenericReparseBuffer content is the basename of the
    underlying pipe.

  Pipe:

    The pipe is named \\.\pipe\cygwin-<installation_key>-unix-[sd]-<uniq_id>

    - <installation_key> is the 8 byte hex Cygwin installation key
    - [sd] is s for SOCK_STREAM, d for SOCK_DGRAM
    - <uniq_id> is an 8 byte hex unique number

   Note: We use MAX_PATH below for convenience where sufficient.  It's
   big enough to hold sun_paths as well as pipe names so we don't have
   to use tmp_pathbuf as often.

   Every packet sent to a peer is a combination of the socket name of the
   local socket, the ancillary data, and the actual user data.  The data
   is always sent in this order.  The header contains length information
   for the entire packet, as well as for all three data blocks.  The
   combined maximum size of a packet is 64K, including the header.

   A connecting, bound STREAM socket sends it's local sun_path once after
   a successful connect.  An already connected socket also sends its local
   sun_path after a successful bind (border case, but still...).  These
   packages don't contain any other data (cmsg_len == 0, data_len == 0).

   A bound DGRAM socket sends its sun_path with each sendmsg/sendto.
*/
class af_unix_pkt_hdr_t
{
 public:
  uint16_t	pckt_len;	/* size of packet including header	*/
  uint8_t	shut_info;	/* shutdown info.  SHUT_RD means
				   SHUT_RD on the local side, so the
				   peer must not send further packets,
				   vice versa for SHUT_WR.   SHUT_RDWR
				   is followed by closing the pipe
				   handle. */
  uint8_t	name_len;	/* size of name, a sockaddr_un		*/
  uint16_t	cmsg_len;	/* size of ancillary data block		*/
  uint16_t	data_len;	/* size of user data			*/

  void init (uint8_t s, uint8_t n, uint16_t c, uint16_t d)
    {
      shut_info = s;
      name_len = n;
      cmsg_len = c;
      data_len = d;
      pckt_len = sizeof (*this) + name_len + cmsg_len + data_len;
    }
};

#define AF_UNIX_PKT_OFFSETOF_NAME(phdr)	\
	(sizeof (af_unix_pkt_hdr_t))
#define AF_UNIX_PKT_OFFSETOF_CMSG(phdr)	\
	(sizeof (af_unix_pkt_hdr_t) + (phdr)->name_len)
#define AF_UNIX_PKT_OFFSETOF_DATA(phdr)	\
	({ \
	   af_unix_pkt_hdr_t *_p = phdr; \
	   sizeof (af_unix_pkt_hdr_t) + (_p)->name_len + (_p)->cmsg_len; \
	})
#define AF_UNIX_PKT_NAME(phdr) \
	({ \
	   af_unix_pkt_hdr_t *_p = phdr; \
	   (struct sockaddr_un *)(((PBYTE)(_p)) \
				  + AF_UNIX_PKT_OFFSETOF_NAME (_p)); \
	})
#define AF_UNIX_PKT_CMSG(phdr) \
	({ \
	   af_unix_pkt_hdr_t *_p = phdr; \
	   (void *)(((PBYTE)(_p)) + AF_UNIX_PKT_OFFSETOF_CMSG (_p)); \
	})
#define AF_UNIX_PKT_DATA(phdr) \
	({ \
	   af_unix_pkt_hdr_t _p = phdr; \
	   (void *)(((PBYTE)(_p)) + AF_UNIX_PKT_OFFSETOF_DATA (_p)); \
	})

GUID __cygwin_socket_guid = {
  .Data1 = 0xefc1714d,
  .Data2 = 0x7b19,
  .Data3 = 0x4407,
  .Data4 = { 0xba, 0xb3, 0xc5, 0xb1, 0xf9, 0x2c, 0xb8, 0x8c }
};

/* Some error conditions on pipes have multiple status codes, unfortunately. */
#define STATUS_PIPE_NO_INSTANCE_AVAILABLE(status)	\
		({ NTSTATUS _s = (status); \
		   _s == STATUS_INSTANCE_NOT_AVAILABLE \
		   || _s == STATUS_PIPE_NOT_AVAILABLE \
		   || _s == STATUS_PIPE_BUSY; })

#define STATUS_PIPE_IS_CLOSING(status)	\
		({ NTSTATUS _s = (status); \
		   _s == STATUS_PIPE_CLOSING \
		   || _s == STATUS_PIPE_EMPTY; })

#define STATUS_PIPE_INVALID(status) \
		({ NTSTATUS _s = (status); \
		   _s == STATUS_INVALID_INFO_CLASS \
		   || _s == STATUS_INVALID_PIPE_STATE \
		   || _s == STATUS_INVALID_READ_MODE; })

#define STATUS_PIPE_MORE_DATA(status) \
		({ NTSTATUS _s = (status); \
		   _s == STATUS_BUFFER_OVERFLOW \
		   || _s == STATUS_MORE_PROCESSING_REQUIRED; })

/* Default timeout value of connect: 20 secs, as on Linux. */
#define AF_UNIX_CONNECT_TIMEOUT (-20 * NS100PERSEC)

sun_name_t::sun_name_t ()
{
  un_len = sizeof (sa_family_t);
  un.sun_family = AF_UNIX;
  _nul[sizeof (struct sockaddr_un)] = '\0';
}

sun_name_t::sun_name_t (const struct sockaddr *name, socklen_t namelen)
{
  if (namelen < 0)
    namelen = 0;
  un_len = namelen < (__socklen_t) sizeof un ? namelen : sizeof un;
  if (name)
    memcpy (&un, name, un_len);
  _nul[sizeof (struct sockaddr_un)] = '\0';
}

static HANDLE
create_event ()
{
  NTSTATUS status;
  OBJECT_ATTRIBUTES attr;
  HANDLE evt = NULL;

  InitializeObjectAttributes (&attr, NULL, 0, NULL, NULL);
  status = NtCreateEvent (&evt, EVENT_ALL_ACCESS, &attr,
			  NotificationEvent, FALSE);
  if (!NT_SUCCESS (status))
    __seterrno_from_nt_status (status);
  return evt;
}

/* Character length of pipe name, excluding trailing NUL. */
#define CYGWIN_PIPE_SOCKET_NAME_LEN     47

/* Character position encoding the socket type in a pipe name. */
#define CYGWIN_PIPE_SOCKET_TYPE_POS	29

void
fhandler_socket_unix::gen_pipe_name ()
{
  WCHAR pipe_name_buf[CYGWIN_PIPE_SOCKET_NAME_LEN + 1];
  UNICODE_STRING pipe_name;

  __small_swprintf (pipe_name_buf, L"cygwin-%S-unix-%C-%016_X",
		    &cygheap->installation_key,
		    get_type_char (),
		    get_plain_ino ());
  RtlInitUnicodeString (&pipe_name, pipe_name_buf);
  pc.set_nt_native_path (&pipe_name);
}

HANDLE
fhandler_socket_unix::create_abstract_link (const sun_name_t *sun,
					    PUNICODE_STRING pipe_name)
{
  WCHAR name[MAX_PATH];
  OBJECT_ATTRIBUTES attr;
  NTSTATUS status;
  UNICODE_STRING uname;
  HANDLE fh = NULL;

  PWCHAR p = wcpcpy (name, L"af-unix-");
  /* NUL bytes have no special meaning in an abstract socket name, so
     we assume iso-8859-1 for simplicity and transpose the string.
     transform_chars_af_unix is doing just that. */
  transform_chars_af_unix (p, sun->un.sun_path, sun->un_len);
  RtlInitUnicodeString (&uname, name);
  InitializeObjectAttributes (&attr, &uname, OBJ_CASE_INSENSITIVE,
			      get_shared_parent_dir (), NULL);
  /* Fill symlink with name of pipe */
  status = NtCreateSymbolicLinkObject (&fh, SYMBOLIC_LINK_ALL_ACCESS,
				       &attr, pipe_name);
  if (!NT_SUCCESS (status))
    {
      if (status == STATUS_OBJECT_NAME_EXISTS
	  || status == STATUS_OBJECT_NAME_COLLISION)
	set_errno (EADDRINUSE);
      else
	__seterrno_from_nt_status (status);
    }
  return fh;
}

struct rep_pipe_name_t
{
  USHORT Length;
  WCHAR  PipeName[1];
};

HANDLE
fhandler_socket_unix::create_reparse_point (const sun_name_t *sun,
					    PUNICODE_STRING pipe_name)
{
  ULONG access;
  HANDLE old_trans = NULL, trans = NULL;
  OBJECT_ATTRIBUTES attr;
  IO_STATUS_BLOCK io;
  NTSTATUS status;
  HANDLE fh = NULL;
  PREPARSE_GUID_DATA_BUFFER rp;
  rep_pipe_name_t *rep_pipe_name;

  const DWORD data_len = offsetof (rep_pipe_name_t, PipeName)
			 + pipe_name->Length + sizeof (WCHAR);

  path_conv pc (sun->un.sun_path, PC_SYM_FOLLOW);
  if (pc.error)
    {
      set_errno (pc.error);
      return NULL;
    }
  if (pc.exists ())
    {
      set_errno (EADDRINUSE);
      return NULL;
    }
 /* We will overwrite the DACL after the call to NtCreateFile.  This
    requires READ_CONTROL and WRITE_DAC access, otherwise get_file_sd
    and set_file_sd both have to open the file again.
    FIXME: On remote NTFS shares open sometimes fails because even the
    creator of the file doesn't have the right to change the DACL.
    I don't know what setting that is or how to recognize such a share,
    so for now we don't request WRITE_DAC on remote drives. */
  access = DELETE | FILE_GENERIC_WRITE;
  if (!pc.isremote ())
    access |= READ_CONTROL | WRITE_DAC | WRITE_OWNER;
  if ((pc.fs_flags () & FILE_SUPPORTS_TRANSACTIONS))
    start_transaction (old_trans, trans);

retry_after_transaction_error:
  status = NtCreateFile (&fh, DELETE | FILE_GENERIC_WRITE,
			 pc.get_object_attr (attr, sec_none_nih), &io,
			 NULL, FILE_ATTRIBUTE_NORMAL, 0, FILE_CREATE,
			 FILE_SYNCHRONOUS_IO_NONALERT
			 | FILE_NON_DIRECTORY_FILE
			 | FILE_OPEN_FOR_BACKUP_INTENT
			 | FILE_OPEN_REPARSE_POINT,
			 NULL, 0);
  if (NT_TRANSACTIONAL_ERROR (status) && trans)
    {
      stop_transaction (status, old_trans, trans);
      goto retry_after_transaction_error;
    }

  if (!NT_SUCCESS (status))
    {
      if (io.Information == FILE_EXISTS)
	set_errno (EADDRINUSE);
      else
	__seterrno_from_nt_status (status);
      goto out;
    }
  rp = (PREPARSE_GUID_DATA_BUFFER)
       alloca (REPARSE_GUID_DATA_BUFFER_HEADER_SIZE + data_len);
  rp->ReparseTag = IO_REPARSE_TAG_CYGUNIX;
  rp->ReparseDataLength = data_len;
  rp->Reserved = 0;
  memcpy (&rp->ReparseGuid, CYGWIN_SOCKET_GUID, sizeof (GUID));
  rep_pipe_name = (rep_pipe_name_t *) rp->GenericReparseBuffer.DataBuffer;
  rep_pipe_name->Length = pipe_name->Length;
  memcpy (rep_pipe_name->PipeName, pipe_name->Buffer, pipe_name->Length);
  rep_pipe_name->PipeName[pipe_name->Length / sizeof (WCHAR)] = L'\0';
  status = NtFsControlFile (fh, NULL, NULL, NULL, &io,
			    FSCTL_SET_REPARSE_POINT, rp,
			    REPARSE_GUID_DATA_BUFFER_HEADER_SIZE
			    + rp->ReparseDataLength, NULL, 0);
  if (NT_SUCCESS (status))
    {
      mode_t perms = (S_IRWXU | S_IRWXG | S_IRWXO) & ~cygheap->umask;
      set_created_file_access (fh, pc, perms);
      NtClose (fh);
      /* We don't have to keep the file open, but the caller needs to
         get a value != NULL to know the file creation went fine. */
      fh = INVALID_HANDLE_VALUE;
    }
  else if (!trans)
    {
      FILE_DISPOSITION_INFORMATION fdi = { TRUE };

      __seterrno_from_nt_status (status);
      status = NtSetInformationFile (fh, &io, &fdi, sizeof fdi,
				     FileDispositionInformation);
      if (!NT_SUCCESS (status))
	debug_printf ("Setting delete dispostion failed, status = %y",
		      status);
      NtClose (fh);
      fh = NULL;
    }

out:
  if (trans)
    stop_transaction (status, old_trans, trans);
  return fh;
}

HANDLE
fhandler_socket_unix::create_file (const sun_name_t *sun)
{
  if (sun->un_len <= (socklen_t) sizeof (sa_family_t)
      || (sun->un_len == 3 && sun->un.sun_path[0] == '\0'))
    {
      set_errno (EINVAL);
      return NULL;
    }
  if (sun->un.sun_path[0] == '\0')
    return create_abstract_link (sun, pc.get_nt_native_path ());
  return create_reparse_point (sun, pc.get_nt_native_path ());
}

int
fhandler_socket_unix::open_abstract_link (sun_name_t *sun,
					  PUNICODE_STRING pipe_name)
{
  WCHAR name[MAX_PATH];
  OBJECT_ATTRIBUTES attr;
  NTSTATUS status;
  UNICODE_STRING uname;
  HANDLE fh;

  PWCHAR p = wcpcpy (name, L"af-unix-");
  p = transform_chars_af_unix (p, sun->un.sun_path, sun->un_len);
  *p = L'\0';
  RtlInitUnicodeString (&uname, name);
  InitializeObjectAttributes (&attr, &uname, OBJ_CASE_INSENSITIVE,
			      get_shared_parent_dir (), NULL);
  status = NtOpenSymbolicLinkObject (&fh, SYMBOLIC_LINK_QUERY, &attr);
  if (!NT_SUCCESS (status))
    {
      __seterrno_from_nt_status (status);
      return -1;
    }
  if (pipe_name)
    status = NtQuerySymbolicLinkObject (fh, pipe_name, NULL);
  NtClose (fh);
  if (pipe_name)
    {
      if (!NT_SUCCESS (status))
	{
	  __seterrno_from_nt_status (status);
	  return -1;
	}
      /* Enforce NUL-terminated pipe name. */
      pipe_name->Buffer[pipe_name->Length / sizeof (WCHAR)] = L'\0';
    }
  return 0;
}

int
fhandler_socket_unix::open_reparse_point (sun_name_t *sun,
					  PUNICODE_STRING pipe_name)
{
  NTSTATUS status;
  HANDLE fh;
  OBJECT_ATTRIBUTES attr;
  IO_STATUS_BLOCK io;
  PREPARSE_GUID_DATA_BUFFER rp;
  tmp_pathbuf tp;

  path_conv pc (sun->un.sun_path, PC_SYM_FOLLOW);
  if (pc.error)
    {
      set_errno (pc.error);
      return -1;
    }
  if (!pc.exists ())
    {
      set_errno (ENOENT);
      return -1;
    }
  pc.get_object_attr (attr, sec_none_nih);
  do
    {
      status = NtOpenFile (&fh, FILE_GENERIC_READ, &attr, &io,
			   FILE_SHARE_VALID_FLAGS,
			   FILE_SYNCHRONOUS_IO_NONALERT
			   | FILE_NON_DIRECTORY_FILE
			   | FILE_OPEN_FOR_BACKUP_INTENT
			   | FILE_OPEN_REPARSE_POINT);
      if (status == STATUS_SHARING_VIOLATION)
        {
          /* While we hope that the sharing violation is only temporary, we
             also could easily get stuck here, waiting for a file in use by
             some greedy Win32 application.  Therefore we should never wait
             endlessly without checking for signals and thread cancel event. */
          pthread_testcancel ();
          if (cygwait (NULL, cw_nowait, cw_sig_eintr) == WAIT_SIGNALED
              && !_my_tls.call_signal_handler ())
            {
              set_errno (EINTR);
              return -1;
            }
          yield ();
        }
      else if (!NT_SUCCESS (status))
        {
          __seterrno_from_nt_status (status);
          return -1;
        }
    }
  while (status == STATUS_SHARING_VIOLATION);
  rp = (PREPARSE_GUID_DATA_BUFFER) tp.c_get ();
  status = NtFsControlFile (fh, NULL, NULL, NULL, &io, FSCTL_GET_REPARSE_POINT,
			    NULL, 0, rp, MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
  NtClose (fh);
  if (rp->ReparseTag == IO_REPARSE_TAG_CYGUNIX
      && memcmp (CYGWIN_SOCKET_GUID, &rp->ReparseGuid, sizeof (GUID)) == 0)
    {
      if (pipe_name)
	{
	  rep_pipe_name_t *rep_pipe_name = (rep_pipe_name_t *)
					   rp->GenericReparseBuffer.DataBuffer;
	  pipe_name->Length = rep_pipe_name->Length;
	  /* pipe name in reparse point is NUL-terminated */
	  memcpy (pipe_name->Buffer, rep_pipe_name->PipeName,
		  rep_pipe_name->Length + sizeof (WCHAR));
	}
      return 0;
    }
  return -1;
}

int
fhandler_socket_unix::open_file (sun_name_t *sun, int &type,
				 PUNICODE_STRING pipe_name)
{
  int ret = -1;

  if (sun->un_len <= (socklen_t) sizeof (sa_family_t)
      || (sun->un_len == 3 && sun->un.sun_path[0] == '\0'))
    set_errno (EINVAL);
  else if (sun->un.sun_path[0] == '\0')
    ret = open_abstract_link (sun, pipe_name);
  else
    ret = open_reparse_point (sun, pipe_name);
  if (!ret)
    switch (pipe_name->Buffer[CYGWIN_PIPE_SOCKET_TYPE_POS])
      {
      case 'd':
	type = SOCK_DGRAM;
	break;
      case 's':
	type = SOCK_STREAM;
	break;
      default:
	set_errno (EINVAL);
	ret = -1;
	break;
      }
  return ret;
}

HANDLE
fhandler_socket_unix::autobind (sun_name_t* sun)
{
  uint32_t id;
  HANDLE fh;

  do
    {
      /* Use only 5 hex digits (up to 2^20 sockets) for Linux compat */
      set_unique_id ();
      id = get_unique_id () & 0xfffff;
      sun->un.sun_path[0] = '\0';
      sun->un_len = sizeof (sa_family_t)
		    + 1 /* leading NUL */
		    + __small_sprintf (sun->un.sun_path + 1, "%5X", id);
    }
  while ((fh = create_abstract_link (sun, pc.get_nt_native_path ())) == NULL);
  return fh;
}

wchar_t
fhandler_socket_unix::get_type_char ()
{
  switch (get_socket_type ())
    {
    case SOCK_STREAM:
      return 's';
    case SOCK_DGRAM:
      return 'd';
    default:
      return '?';
    }
}

/* This also sets the pipe to message mode unconditionally. */
void
fhandler_socket_unix::set_pipe_non_blocking (bool nonblocking)
{
  if (get_handle ())
    {
      NTSTATUS status;
      IO_STATUS_BLOCK io;
      FILE_PIPE_INFORMATION fpi;

      fpi.ReadMode = FILE_PIPE_MESSAGE_MODE;
      fpi.CompletionMode = nonblocking ? FILE_PIPE_COMPLETE_OPERATION
				       : FILE_PIPE_QUEUE_OPERATION;
      status = NtSetInformationFile (get_handle (), &io, &fpi, sizeof fpi,
				     FilePipeInformation);
      if (!NT_SUCCESS (status))
	debug_printf ("NtSetInformationFile(FilePipeInformation): %y", status);
    }
}

int
fhandler_socket_unix::send_my_name ()
{
  sun_name_t *sun;
  size_t name_len = 0;
  af_unix_pkt_hdr_t *packet;
  NTSTATUS status;
  IO_STATUS_BLOCK io;

  AcquireSRWLockShared (&bind_lock);
  sun = get_sun_path ();
  name_len = sun ? sun->un_len : 0;
  packet = (af_unix_pkt_hdr_t *) alloca (sizeof *packet + name_len);
  if (sun)
    memcpy (AF_UNIX_PKT_NAME (packet), &sun->un, name_len);
  ReleaseSRWLockShared (&bind_lock);

  packet->init (0, name_len, 0, 0);

  /* The theory: Fire and forget. */
  AcquireSRWLockExclusive (&io_lock);
  set_pipe_non_blocking (true);
  status = NtWriteFile (get_handle (), NULL, NULL, NULL, &io, packet,
			packet->pckt_len, NULL, NULL);
  set_pipe_non_blocking (is_nonblocking ());
  ReleaseSRWLockExclusive (&io_lock);
  if (!NT_SUCCESS (status))
    {
      debug_printf ("Couldn't send my name: NtWriteFile: %y", status);
      return -1;
    }
  return 0;
}

/* Returns an error code.  Locking is not required, user space doesn't know
   about this socket yet. */
int
fhandler_socket_unix::recv_peer_name ()
{
  HANDLE evt;
  NTSTATUS status;
  IO_STATUS_BLOCK io;
  af_unix_pkt_hdr_t *packet;
  struct sockaddr_un *un;
  ULONG len;
  int ret = 0;

  if (!(evt = create_event ()))
    return ENOBUFS;
  len = sizeof *packet + sizeof *un;
  packet = (af_unix_pkt_hdr_t *) alloca (len);
  set_pipe_non_blocking (false);
  status = NtReadFile (get_handle (), evt, NULL, NULL, &io, packet, len,
		       NULL, NULL);
  if (status == STATUS_PENDING)
    {
      DWORD ret;
      LARGE_INTEGER timeout;

      timeout.QuadPart = AF_UNIX_CONNECT_TIMEOUT;
      ret = cygwait (evt, &timeout, cw_sig_eintr);
      switch (ret)
	{
	case WAIT_OBJECT_0:
	  status = io.Status;
	  break;
	case WAIT_TIMEOUT:
	  ret = ECONNABORTED;
	  break;
	case WAIT_SIGNALED:
	  ret = EINTR;
	  break;
	default:
	  ret = EPROTO;
	  break;
	}
    }
  if (!NT_SUCCESS (status) && ret == 0)
    ret = geterrno_from_nt_status (status);
  if (ret == 0 && packet->name_len > 0)
    set_peer_sun_path (AF_UNIX_PKT_NAME (packet), packet->name_len);
  set_pipe_non_blocking (is_nonblocking ());
  return ret;
}

NTSTATUS
fhandler_socket_unix::npfs_handle (HANDLE &nph)
{
  static NO_COPY SRWLOCK npfs_lock;
  static NO_COPY HANDLE npfs_dirh;

  NTSTATUS status = STATUS_SUCCESS;
  OBJECT_ATTRIBUTES attr;
  IO_STATUS_BLOCK io;

  /* Lockless after first call. */
  if (npfs_dirh)
    {
      nph = npfs_dirh;
      return STATUS_SUCCESS;
    }
  AcquireSRWLockExclusive (&npfs_lock);
  if (!npfs_dirh)
    {
      InitializeObjectAttributes (&attr, &ro_u_npfs, 0, NULL, NULL);
      status = NtOpenFile (&npfs_dirh, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
			   &attr, &io, FILE_SHARE_READ | FILE_SHARE_WRITE,
			   0);
    }
  ReleaseSRWLockExclusive (&npfs_lock);
  if (NT_SUCCESS (status))
    nph = npfs_dirh;
  return status;
}

HANDLE
fhandler_socket_unix::create_pipe ()
{
  NTSTATUS status;
  HANDLE npfsh;
  HANDLE ph;
  ACCESS_MASK access;
  OBJECT_ATTRIBUTES attr;
  IO_STATUS_BLOCK io;
  ULONG sharing;
  ULONG nonblocking;
  ULONG max_instances;
  LARGE_INTEGER timeout;

  status = npfs_handle (npfsh);
  if (!NT_SUCCESS (status))
    {
      __seterrno_from_nt_status (status);
      return NULL;
    }
  access = GENERIC_READ | FILE_READ_ATTRIBUTES
	   | GENERIC_WRITE |  FILE_WRITE_ATTRIBUTES
	   | SYNCHRONIZE;
  sharing = FILE_SHARE_READ | FILE_SHARE_WRITE;
  InitializeObjectAttributes (&attr, pc.get_nt_native_path (),
			      OBJ_INHERIT | OBJ_CASE_INSENSITIVE,
			      npfsh, NULL);
  nonblocking = is_nonblocking () ? FILE_PIPE_COMPLETE_OPERATION
				  : FILE_PIPE_QUEUE_OPERATION;
  max_instances = (get_socket_type () == SOCK_DGRAM) ? 1 : -1;
  timeout.QuadPart = -500000;
  status = NtCreateNamedPipeFile (&ph, access, &attr, &io, sharing,
				  FILE_CREATE, 0,
				  FILE_PIPE_MESSAGE_TYPE,
				  FILE_PIPE_MESSAGE_MODE,
				  nonblocking, max_instances,
				  rmem (), wmem (), &timeout);
  if (!NT_SUCCESS (status))
    __seterrno_from_nt_status (status);
  return ph;
}

HANDLE
fhandler_socket_unix::create_pipe_instance ()
{
  NTSTATUS status;
  HANDLE npfsh;
  HANDLE ph;
  ACCESS_MASK access;
  OBJECT_ATTRIBUTES attr;
  IO_STATUS_BLOCK io;
  ULONG sharing;
  ULONG nonblocking;
  ULONG max_instances;
  LARGE_INTEGER timeout;

  status = npfs_handle (npfsh);
  if (!NT_SUCCESS (status))
    {
      __seterrno_from_nt_status (status);
      return NULL;
    }
  access = GENERIC_READ | FILE_READ_ATTRIBUTES
	   | GENERIC_WRITE |  FILE_WRITE_ATTRIBUTES
	   | SYNCHRONIZE;
  sharing = FILE_SHARE_READ | FILE_SHARE_WRITE;
  /* NPFS doesn't understand reopening by handle, unfortunately. */
  InitializeObjectAttributes (&attr, pc.get_nt_native_path (), OBJ_INHERIT,
			      npfsh, NULL);
  nonblocking = is_nonblocking () ? FILE_PIPE_COMPLETE_OPERATION
				  : FILE_PIPE_QUEUE_OPERATION;
  max_instances = (get_socket_type () == SOCK_DGRAM) ? 1 : -1;
  timeout.QuadPart = -500000;
  status = NtCreateNamedPipeFile (&ph, access, &attr, &io, sharing,
				  FILE_OPEN, 0,
				  FILE_PIPE_MESSAGE_TYPE,
				  FILE_PIPE_MESSAGE_MODE,
				  nonblocking, max_instances,
				  rmem (), wmem (), &timeout);
  if (!NT_SUCCESS (status))
    __seterrno_from_nt_status (status);
  return ph;
}

NTSTATUS
fhandler_socket_unix::open_pipe (HANDLE &ph, PUNICODE_STRING pipe_name)
{
  NTSTATUS status;
  HANDLE npfsh;
  ACCESS_MASK access;
  OBJECT_ATTRIBUTES attr;
  IO_STATUS_BLOCK io;
  ULONG sharing;

  status = npfs_handle (npfsh);
  if (!NT_SUCCESS (status))
    return status;
  access = GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE;
  InitializeObjectAttributes (&attr, pipe_name, OBJ_INHERIT, npfsh, NULL);
  sharing = FILE_SHARE_READ | FILE_SHARE_WRITE;
  status = NtOpenFile (&ph, access, &attr, &io, sharing, 0);
  if (NT_SUCCESS (status))
    {
      set_io_handle (ph);
      send_my_name ();
    }
  return status;
}

struct conn_wait_info_t
{
  fhandler_socket_unix *fh;
  UNICODE_STRING pipe_name;
  WCHAR pipe_name_buf[CYGWIN_PIPE_SOCKET_NAME_LEN + 1];
};

/* Just hop to the wait_pipe_thread method. */
DWORD WINAPI
connect_wait_func (LPVOID param)
{
  conn_wait_info_t *wait_info = (conn_wait_info_t *) param;
  return wait_info->fh->wait_pipe_thread (&wait_info->pipe_name);
}

/* Start a waiter thread to wait for a pipe instance to become available.
   in blocking mode, wait for the thread to finish.  In nonblocking mode
   just return with errno set to EINPROGRESS. */
int
fhandler_socket_unix::wait_pipe (PUNICODE_STRING pipe_name)
{
  conn_wait_info_t *wait_info;
  DWORD waitret, err;
  int ret = -1;
  HANDLE thr, evt;
  PVOID param;

  if (!(cwt_termination_evt = create_event ()))
      return -1;
  wait_info = (conn_wait_info_t *)
	      cmalloc_abort (HEAP_FHANDLER, sizeof *wait_info);
  wait_info->fh = this;
  RtlInitEmptyUnicodeString (&wait_info->pipe_name, wait_info->pipe_name_buf,
			     sizeof wait_info->pipe_name_buf);
  RtlCopyUnicodeString (&wait_info->pipe_name, pipe_name);

  cwt_param = (PVOID) wait_info;
  connect_wait_thr = CreateThread (NULL, PREFERRED_IO_BLKSIZE,
				   connect_wait_func, cwt_param, 0, NULL);
  if (!connect_wait_thr)
    {
      cfree (wait_info);
      __seterrno ();
      goto out;
    }
  if (is_nonblocking ())
    {
      set_errno (EINPROGRESS);
      goto out;
    }

  waitret = cygwait (connect_wait_thr, cw_infinite, cw_cancel | cw_sig_eintr);
  if (waitret == WAIT_OBJECT_0)
    GetExitCodeThread (connect_wait_thr, &err);
  else
    {
      SetEvent (cwt_termination_evt);
      WaitForSingleObject (connect_wait_thr, INFINITE);
      GetExitCodeThread (connect_wait_thr, &err);
      waitret = WAIT_SIGNALED;
    }
  thr = InterlockedExchangePointer (&connect_wait_thr, NULL);
  if (thr)
    CloseHandle (thr);
  param = InterlockedExchangePointer (&cwt_param, NULL);
  if (param)
    cfree (param);
  switch (waitret)
    {
    case WAIT_CANCELED:
      pthread::static_cancel_self ();
      /*NOTREACHED*/
    case WAIT_SIGNALED:
      set_errno (EINTR);
      break;
    default:
      InterlockedExchange (&so_error, err);
      if (err)
	set_errno (err);
      else
	ret = 0;
      break;
    }
out:
  evt = InterlockedExchangePointer (&cwt_termination_evt, NULL);
  if (evt)
    NtClose (evt);
  return ret;
}

int
fhandler_socket_unix::connect_pipe (PUNICODE_STRING pipe_name)
{
  NTSTATUS status;
  HANDLE ph = NULL;

  /* Try connecting first.  If it doesn't work, wait for the pipe
     to become available. */
  status = open_pipe (ph, pipe_name);
  if (STATUS_PIPE_NO_INSTANCE_AVAILABLE (status))
    return wait_pipe (pipe_name);
  if (!NT_SUCCESS (status))
    {
      __seterrno_from_nt_status (status);
      InterlockedExchange (&so_error, get_errno ());
      return -1;
    }
  InterlockedExchange (&so_error, 0);
  return 0;
}

int
fhandler_socket_unix::listen_pipe ()
{
  NTSTATUS status;
  IO_STATUS_BLOCK io;
  HANDLE evt = NULL;
  DWORD waitret = WAIT_OBJECT_0;

  io.Status = STATUS_PENDING;
  if (!is_nonblocking () && !(evt = create_event ()))
    return -1;
  status = NtFsControlFile (get_handle (), evt, NULL, NULL, &io,
			    FSCTL_PIPE_LISTEN, NULL, 0, NULL, 0);
  if (status == STATUS_PENDING)
    {
      waitret = cygwait (evt ?: get_handle (), cw_infinite,
			 cw_cancel | cw_sig_eintr);
      if (waitret == WAIT_OBJECT_0)
	status = io.Status;
    }
  if (evt)
    NtClose (evt);
  if (waitret == WAIT_CANCELED)
    pthread::static_cancel_self ();
  else if (waitret == WAIT_SIGNALED)
    set_errno (EINTR);
  else if (status == STATUS_PIPE_LISTENING)
    set_errno (EAGAIN);
  else if (status != STATUS_PIPE_CONNECTED)
    __seterrno_from_nt_status (status);
  return (status == STATUS_PIPE_CONNECTED) ? 0 : -1;
}

int
fhandler_socket_unix::disconnect_pipe (HANDLE ph)
{
  NTSTATUS status;
  IO_STATUS_BLOCK io;

  status = NtFsControlFile (ph, NULL, NULL, NULL, &io, FSCTL_PIPE_DISCONNECT,
			    NULL, 0, NULL, 0);
  /* Short-lived.  Don't use cygwait.  We don't want to be interrupted. */
  if (status == STATUS_PENDING
      && WaitForSingleObject (ph, INFINITE) == WAIT_OBJECT_0)
    status = io.Status;
  if (!NT_SUCCESS (status))
    {
      __seterrno_from_nt_status (status);
      return -1;
    }
  return 0;
}

void
fhandler_socket_unix::set_sun_path (struct sockaddr_un *un, socklen_t unlen)
{
  if (peer_sun_path)
    delete peer_sun_path;
  if (!un)
    sun_path = NULL;
  sun_path = new sun_name_t ((const struct sockaddr *) un, unlen);
}

void
fhandler_socket_unix::set_peer_sun_path (struct sockaddr_un *un,
					 socklen_t unlen)
{
  if (peer_sun_path)
    delete peer_sun_path;
  if (!un)
    peer_sun_path = NULL;
  peer_sun_path = new sun_name_t ((const struct sockaddr *) un, unlen);
}

void
fhandler_socket_unix::set_cred ()
{
  peer_cred.pid = (pid_t) 0;
  peer_cred.uid = (uid_t) -1;
  peer_cred.gid = (gid_t) -1;
}

void
fhandler_socket_unix::fixup_after_fork (HANDLE parent)
{
  fhandler_socket::fixup_after_fork (parent);
  if (backing_file_handle && backing_file_handle != INVALID_HANDLE_VALUE)
    fork_fixup (parent, backing_file_handle, "backing_file_handle");
  InitializeSRWLock (&conn_lock);
  InitializeSRWLock (&bind_lock);
  InitializeSRWLock (&io_lock);
  connect_wait_thr = NULL;
  cwt_termination_evt = NULL;
  cwt_param = NULL;
}

void
fhandler_socket_unix::set_close_on_exec (bool val)
{
  fhandler_base::set_close_on_exec (val);
  if (backing_file_handle && backing_file_handle != INVALID_HANDLE_VALUE)
    set_no_inheritance (backing_file_handle, val);
}

/* ========================== public methods ========================= */

fhandler_socket_unix::fhandler_socket_unix ()
{
  set_cred ();
}

fhandler_socket_unix::~fhandler_socket_unix ()
{
  if (sun_path)
    delete sun_path;
  if (peer_sun_path)
    delete peer_sun_path;
}

int
fhandler_socket_unix::dup (fhandler_base *child, int flags)
{
  fhandler_socket_unix *fhs = (fhandler_socket_unix *) child;
  fhs->set_sun_path (get_sun_path ());
  fhs->set_peer_sun_path (get_peer_sun_path ());
  InitializeSRWLock (&fhs->conn_lock);
  InitializeSRWLock (&fhs->bind_lock);
  InitializeSRWLock (&fhs->io_lock);
  fhs->connect_wait_thr = NULL;
  fhs->cwt_termination_evt = NULL;
  fhs->cwt_param = NULL;
  return fhandler_socket::dup (child, flags);
}

/* Waiter thread method.  Here we wait for a pipe instance to become
   available and connect to it, if so.  This function is running
   asynchronously if called on a non-blocking pipe.  The important
   things to do:

   - Set the peer pipe handle if successful
   - Send own sun_path to peer if successful
   - Set connect_state
   - Set so_error for later call to select
*/
DWORD
fhandler_socket_unix::wait_pipe_thread (PUNICODE_STRING pipe_name)
{
  HANDLE npfsh;
  HANDLE evt;
  LONG error = 0;
  NTSTATUS status;
  IO_STATUS_BLOCK io;
  ULONG pwbuf_size;
  PFILE_PIPE_WAIT_FOR_BUFFER pwbuf;
  LONGLONG stamp;
  HANDLE ph = NULL;

  status = npfs_handle (npfsh);
  if (!NT_SUCCESS (status))
    {
      error = geterrno_from_nt_status (status);
      goto out;
    }
  if (!(evt = create_event ()))
    goto out;
  pwbuf_size = offsetof (FILE_PIPE_WAIT_FOR_BUFFER, Name) + pipe_name->Length;
  pwbuf = (PFILE_PIPE_WAIT_FOR_BUFFER) alloca (pwbuf_size);
  pwbuf->Timeout.QuadPart = AF_UNIX_CONNECT_TIMEOUT;
  pwbuf->NameLength = pipe_name->Length;
  pwbuf->TimeoutSpecified = TRUE;
  memcpy (pwbuf->Name, pipe_name->Buffer, pipe_name->Length);
  stamp = ntod.nsecs ();
  do
    {
      status = NtFsControlFile (npfsh, evt, NULL, NULL, &io, FSCTL_PIPE_WAIT,
				pwbuf, pwbuf_size, NULL, 0);
      if (status == STATUS_PENDING)
	{
	  HANDLE w[2] = { evt, cwt_termination_evt };
	  switch (WaitForMultipleObjects (2, w, FALSE, INFINITE))
	    {
	    case WAIT_OBJECT_0:
	      status = io.Status;
	      break;
	    case WAIT_OBJECT_0 + 1:
	    default:
	      status = STATUS_THREAD_IS_TERMINATING;
	      break;
	    }
	}
      switch (status)
	{
	  case STATUS_SUCCESS:
	    {
	      status = open_pipe (ph, pipe_name);
	      if (STATUS_PIPE_NO_INSTANCE_AVAILABLE (status))
		{
		  /* Another concurrent connect grabbed the pipe instance
		     under our nose.  Fix the timeout value and go waiting
		     again, unless the timeout has passed. */
		  pwbuf->Timeout.QuadPart -= (stamp - ntod.nsecs ()) / 100LL;
		  if (pwbuf->Timeout.QuadPart >= 0)
		    {
		      status = STATUS_IO_TIMEOUT;
		      error = ETIMEDOUT;
		    }
		}
	      else if (!NT_SUCCESS (status))
		error = geterrno_from_nt_status (status);
	    }
	    break;
	  case STATUS_OBJECT_NAME_NOT_FOUND:
	    error = EADDRNOTAVAIL;
	    break;
	  case STATUS_IO_TIMEOUT:
	    error = ETIMEDOUT;
	    break;
	  case STATUS_INSUFFICIENT_RESOURCES:
	    error = ENOBUFS;
	    break;
	  case STATUS_THREAD_IS_TERMINATING:
	    error = EINTR;
	    break;
	  case STATUS_INVALID_DEVICE_REQUEST:
	  default:
	    error = EIO;
	    break;
	}
    }
  while (STATUS_PIPE_NO_INSTANCE_AVAILABLE (status));
out:
  PVOID param = InterlockedExchangePointer (&cwt_param, NULL);
  if (param)
    cfree (param);
  AcquireSRWLockExclusive (&conn_lock);
  InterlockedExchange (&so_error, error);
  connect_state (error ? connect_failed : connected);
  ReleaseSRWLockExclusive (&conn_lock);
  return error;
}

int
fhandler_socket_unix::socket (int af, int type, int protocol, int flags)
{
  if (type != SOCK_STREAM && type != SOCK_DGRAM)
    {
      set_errno (EINVAL);
      return -1;
    }
  if (protocol != 0)
    {
      set_errno (EPROTONOSUPPORT);
      return -1;
    }
  rmem (262144);
  wmem (262144);
  set_addr_family (af);
  set_socket_type (type);
  if (flags & SOCK_NONBLOCK)
    set_nonblocking (true);
  if (flags & SOCK_CLOEXEC)
    set_close_on_exec (true);
  set_io_handle (NULL);
  set_unique_id ();
  set_ino (get_unique_id ());
  return 0;
}

int
fhandler_socket_unix::socketpair (int af, int type, int protocol, int flags,
				  fhandler_socket *fh_out)
{
  if (type != SOCK_STREAM && type != SOCK_DGRAM)
    {
      set_errno (EINVAL);
      return -1;
    }
  if (protocol != 0)
    {
      set_errno (EPROTONOSUPPORT);
      return -1;
    }
  set_errno (EAFNOSUPPORT);
  return -1;
}

/* Bind creates the backing file, generates the pipe name and sets
   bind_state.  On DGRAM sockets it also creates the pipe.  On STREAM
   sockets either listen or connect will do that. */
int
fhandler_socket_unix::bind (const struct sockaddr *name, int namelen)
{
  sun_name_t sun (name, namelen);
  bool unnamed = (sun.un_len == sizeof sun.un.sun_family);
  HANDLE pipe = NULL;

  if (sun.un.sun_family != AF_UNIX)
    {
      set_errno (EINVAL);
      return -1;
    }
  AcquireSRWLockExclusive (&bind_lock);
  if (binding_state () == bind_pending)
    {
      set_errno (EALREADY);
      ReleaseSRWLockExclusive (&bind_lock);
      return -1;
    }
  if (binding_state () == bound)
    {
      set_errno (EINVAL);
      ReleaseSRWLockExclusive (&bind_lock);
      return -1;
    }
  binding_state (bind_pending);
  ReleaseSRWLockExclusive (&bind_lock);
  gen_pipe_name ();
  if (get_socket_type () == SOCK_DGRAM)
    {
      pipe = create_pipe ();
      if (!pipe)
	{
	  binding_state (unbound);
	  return -1;
	}
      set_io_handle (pipe);
    }
  backing_file_handle = unnamed ? autobind (&sun) : create_file (&sun);
  if (!backing_file_handle)
    {
      set_io_handle (NULL);
      if (pipe)
	NtClose (pipe);
      binding_state (unbound);
      return -1;
    }
  set_sun_path (&sun);
  /* If we're already connected, send name to peer. */
  if (connect_state () == connected)
    send_my_name ();
  binding_state (bound);
  return 0;
}

/* Create pipe on non-DGRAM sockets and set conn_state to listener. */
int
fhandler_socket_unix::listen (int backlog)
{
  if (get_socket_type () == SOCK_DGRAM)
    {
      set_errno (EOPNOTSUPP);
      return -1;
    }
  AcquireSRWLockShared (&bind_lock);
  while (binding_state () == bind_pending)
    yield ();
  if (binding_state () == unbound)
    {
      set_errno (EDESTADDRREQ);
      ReleaseSRWLockShared (&bind_lock);
      return -1;
    }
  ReleaseSRWLockShared (&bind_lock);
  AcquireSRWLockExclusive (&conn_lock);
  if (connect_state () != unconnected && connect_state () != connect_failed)
    {
      set_errno (connect_state () == listener ? EADDRINUSE : EINVAL);
      ReleaseSRWLockExclusive (&conn_lock);
      return -1;
    }
  if (get_socket_type () != SOCK_DGRAM)
    {
      HANDLE pipe = create_pipe ();
      if (!pipe)
	{
	  connect_state (unconnected);
	  return -1;
	}
      set_io_handle (pipe);
    }
  connect_state (listener);
  ReleaseSRWLockExclusive (&conn_lock);
  return 0;
}

int
fhandler_socket_unix::accept4 (struct sockaddr *peer, int *len, int flags)
{
  if (get_socket_type () != SOCK_STREAM)
    {
      set_errno (EOPNOTSUPP);
      return -1;
    }
  if (connect_state () != listener
      || (peer && (!len || *len < (int) sizeof (sa_family_t))))
    {
      set_errno (EINVAL);
      return -1;
    }
  if (listen_pipe () == 0)
    {
      /* Our handle is now connected with a client.  This handle is used
         for the accepted socket.  Our handle has to be replaced with a
	 new instance handle for the next accept. */
      AcquireSRWLockExclusive (&io_lock);
      HANDLE accepted = get_handle ();
      HANDLE new_inst = create_pipe_instance ();
      int error = ENOBUFS;
      if (!new_inst)
	ReleaseSRWLockExclusive (&io_lock);
      else
	{
	  /* Set new io handle. */
	  set_io_handle (new_inst);
	  ReleaseSRWLockExclusive (&io_lock);
	  /* Prepare new file descriptor. */
	  cygheap_fdnew fd;

	  if (fd >= 0)
	    {
	      fhandler_socket_unix *sock = (fhandler_socket_unix *)
					   build_fh_dev (dev ());
	      if (sock)
		{
		  sock->set_addr_family (get_addr_family ());
		  sock->set_socket_type (get_socket_type ());
		  if (flags & SOCK_NONBLOCK)
		    sock->set_nonblocking (true);
		  if (flags & SOCK_CLOEXEC)
		    sock->set_close_on_exec (true);
		  sock->set_unique_id ();
		  sock->set_ino (sock->get_unique_id ());
		  sock->pc.set_nt_native_path (pc.get_nt_native_path ());
		  sock->connect_state (connected);
		  sock->binding_state (binding_state ());
		  sock->set_io_handle (accepted);

		  sock->set_sun_path (get_sun_path ());
		  error = sock->recv_peer_name ();
		  if (error == 0)
		    {
		      __try
			{
			  if (peer)
			    {
			      sun_name_t *sun = sock->get_peer_sun_path ();
			      if (sun)
				{
				  memcpy (peer, &sun->un,
					  MIN (*len, sun->un_len));
				  *len = sun->un_len;
				}
			      else if (len)
				*len = 0;
			    }
			  fd = sock;
			  if (fd <= 2)
			    set_std_handle (fd);
			  return fd;
			}
		      __except (NO_ERROR)
			{
			  error = EFAULT;
			}
		      __endtry
		    }
		  delete sock;
		}
	      fd.release ();
	    }
	}
      /* Ouch!  We can't handle the client if we couldn't
	 create a new instance to accept more connections.*/
      disconnect_pipe (accepted);
      set_errno (error);
    }
  return -1;
}

int
fhandler_socket_unix::connect (const struct sockaddr *name, int namelen)
{
  sun_name_t sun (name, namelen);
  int peer_type;
  WCHAR pipe_name_buf[CYGWIN_PIPE_SOCKET_NAME_LEN + 1];
  UNICODE_STRING pipe_name;

  /* Test and set connection state. */
  AcquireSRWLockExclusive (&conn_lock);
  if (connect_state () == connect_pending)
    {
      set_errno (EALREADY);
      ReleaseSRWLockExclusive (&conn_lock);
      return -1;
    }
  if (connect_state () == listener)
    {
      set_errno (EADDRINUSE);
      ReleaseSRWLockExclusive (&conn_lock);
      return -1;
    }
  if (connect_state () == connected && get_socket_type () != SOCK_DGRAM)
    {
      set_errno (EISCONN);
      ReleaseSRWLockExclusive (&conn_lock);
      return -1;
    }
  connect_state (connect_pending);
  ReleaseSRWLockExclusive (&conn_lock);
  /* Check validity of name */
  if (sun.un_len <= (int) sizeof (sa_family_t))
    {
      set_errno (EINVAL);
      connect_state (unconnected);
      return -1;
    }
  if (sun.un.sun_family != AF_UNIX)
    {
      set_errno (EAFNOSUPPORT);
      connect_state (unconnected);
      return -1;
    }
  if (sun.un_len == 3 && sun.un.sun_path[0] == '\0')
    {
      set_errno (EINVAL);
      connect_state (unconnected);
      return -1;
    }
  /* Check if peer address exists. */
  RtlInitEmptyUnicodeString (&pipe_name, pipe_name_buf, sizeof pipe_name_buf);
  if (open_file (&sun, peer_type, &pipe_name) < 0)
    {
      connect_state (unconnected);
      return -1;
    }
  if (peer_type != get_socket_type ())
    {
      set_errno (EINVAL);
      connect_state (unconnected);
      return -1;
    }
  set_peer_sun_path (&sun);
  if (get_socket_type () != SOCK_DGRAM)
    {
      if (connect_pipe (&pipe_name) < 0)
	{
	  if (get_errno () != EINPROGRESS)
	    {
	      set_peer_sun_path (NULL);
	      connect_state (connect_failed);
	    }
	  return -1;
	}
    }
  connect_state (connected);
  return 0;
}

int
fhandler_socket_unix::getsockname (struct sockaddr *name, int *namelen)
{
  sun_name_t sun;

  if (get_sun_path ())
    memcpy (&sun, &get_sun_path ()->un, get_sun_path ()->un_len);
  else
    sun.un_len = 0;
  memcpy (name, &sun, MIN (*namelen, sun.un_len));
  *namelen = sun.un_len;
  return 0;
}

int
fhandler_socket_unix::getpeername (struct sockaddr *name, int *namelen)
{
  sun_name_t sun;

  if (get_peer_sun_path ())
    memcpy (&sun, &get_peer_sun_path ()->un, get_peer_sun_path ()->un_len);
  else
    sun.un_len = 0;
  memcpy (name, &sun, MIN (*namelen, sun.un_len));
  *namelen = sun.un_len;
  return 0;
}

int
fhandler_socket_unix::shutdown (int how)
{
  set_errno (EAFNOSUPPORT);
  return -1;
}

int
fhandler_socket_unix::close ()
{
  HANDLE evt = InterlockedExchangePointer (&cwt_termination_evt, NULL);
  HANDLE thr = InterlockedExchangePointer (&connect_wait_thr, NULL);
  if (thr)
    {
      if (evt)
	SetEvent (evt);
      WaitForSingleObject (thr, INFINITE);
      CloseHandle (thr);
    }
  if (evt)
    NtClose (evt);
  PVOID param = InterlockedExchangePointer (&cwt_param, NULL);
  if (param)
    cfree (param);
  if (get_handle ())
    NtClose (get_handle ());
  if (backing_file_handle && backing_file_handle != INVALID_HANDLE_VALUE)
    NtClose (backing_file_handle);
  return 0;
}

int
fhandler_socket_unix::getpeereid (pid_t *pid, uid_t *euid, gid_t *egid)
{
  int ret = -1;

  if (get_socket_type () != SOCK_STREAM)
    {
      set_errno (EINVAL);
      return -1;
    }
  AcquireSRWLockShared (&conn_lock);
  if (connect_state () != connected)
    set_errno (ENOTCONN);
  else
    {
      __try
	{
	  if (pid)
	    *pid = peer_cred.pid;
	  if (euid)
	    *euid = peer_cred.uid;
	  if (egid)
	    *egid = peer_cred.gid;
	  ret = 0;
	}
      __except (EFAULT) {}
      __endtry
    }
  ReleaseSRWLockShared (&conn_lock);
  return ret;
}

ssize_t
fhandler_socket_unix::recvmsg (struct msghdr *msg, int flags)
{
  set_errno (EAFNOSUPPORT);
  return -1;
}

ssize_t
fhandler_socket_unix::recvfrom (void *ptr, size_t len, int flags,
				struct sockaddr *from, int *fromlen)
{
  struct iovec iov;
  struct msghdr msg;
  ssize_t ret;

  iov.iov_base = ptr;
  iov.iov_len = len;
  msg.msg_name = from;
  msg.msg_namelen = from && fromlen ? *fromlen : 0;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = NULL;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;
  ret = recvmsg (&msg, flags);
  if (ret >= 0 && from && fromlen)
    *fromlen = msg.msg_namelen;
  return ret;
}

void __reg3
fhandler_socket_unix::read (void *ptr, size_t& len)
{
  set_errno (EAFNOSUPPORT);
  len = 0;
  struct iovec iov;
  struct msghdr msg;

  iov.iov_base = ptr;
  iov.iov_len = len;
  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = NULL;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;
  len = recvmsg (&msg, 0);
}

ssize_t __stdcall
fhandler_socket_unix::readv (const struct iovec *const iov, int iovcnt,
			     ssize_t tot)
{
  struct msghdr msg;

  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  msg.msg_iov = (struct iovec *) iov;
  msg.msg_iovlen = iovcnt;
  msg.msg_control = NULL;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;
  return recvmsg (&msg, 0);
}

ssize_t
fhandler_socket_unix::sendmsg (const struct msghdr *msg, int flags)
{
  set_errno (EAFNOSUPPORT);
  return -1;
}

ssize_t
fhandler_socket_unix::sendto (const void *in_ptr, size_t len, int flags,
			       const struct sockaddr *to, int tolen)
{
  struct iovec iov;
  struct msghdr msg;

  iov.iov_base = (void *) in_ptr;
  iov.iov_len = len;
  msg.msg_name = (void *) to;
  msg.msg_namelen = to ? tolen : 0;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = NULL;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;
  return sendmsg (&msg, flags);
}

ssize_t __stdcall
fhandler_socket_unix::write (const void *ptr, size_t len)
{
  struct iovec iov;
  struct msghdr msg;

  iov.iov_base = (void *) ptr;
  iov.iov_len = len;
  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = NULL;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;
  return sendmsg (&msg, 0);
}

ssize_t __stdcall
fhandler_socket_unix::writev (const struct iovec *const iov, int iovcnt,
			      ssize_t tot)
{
  struct msghdr msg;

  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  msg.msg_iov = (struct iovec *) iov;
  msg.msg_iovlen = iovcnt;
  msg.msg_control = NULL;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;
  return sendmsg (&msg, 0);
}

int
fhandler_socket_unix::setsockopt (int level, int optname, const void *optval,
				   socklen_t optlen)
{
  /* Preprocessing setsockopt. */
  switch (level)
    {
    case SOL_SOCKET:
      switch (optname)
	{
	case SO_PASSCRED:
	  break;

	case SO_REUSEADDR:
	  saw_reuseaddr (*(int *) optval);
	  break;

	case SO_RCVBUF:
	  rmem (*(int *) optval);
	  break;

	case SO_SNDBUF:
	  wmem (*(int *) optval);
	  break;

	case SO_RCVTIMEO:
	case SO_SNDTIMEO:
	  if (optlen < (socklen_t) sizeof (struct timeval))
	    {
	      set_errno (EINVAL);
	      return -1;
	    }
	  if (!timeval_to_ms ((struct timeval *) optval,
			      (optname == SO_RCVTIMEO) ? rcvtimeo ()
						       : sndtimeo ()))
	  {
	    set_errno (EDOM);
	    return -1;
	  }
	  break;

	default:
	  /* AF_UNIX sockets simply ignore all other SOL_SOCKET options. */
	  break;
	}
      break;

    default:
      set_errno (ENOPROTOOPT);
      return -1;
    }

  return 0;
}

int
fhandler_socket_unix::getsockopt (int level, int optname, const void *optval,
				   socklen_t *optlen)
{
  /* Preprocessing getsockopt.*/
  switch (level)
    {
    case SOL_SOCKET:
      switch (optname)
	{
	case SO_ERROR:
	  {
	    int *e = (int *) optval;
	    LONG err;

	    err = InterlockedExchange (&so_error, 0);
	    *e = err;
	    break;
	  }

	case SO_PASSCRED:
	  break;

	case SO_PEERCRED:
	  {
	    struct ucred *cred = (struct ucred *) optval;

	    if (*optlen < (socklen_t) sizeof *cred)
	      {
		set_errno (EINVAL);
		return -1;
	      }
	    int ret = getpeereid (&cred->pid, &cred->uid, &cred->gid);
	    if (!ret)
	      *optlen = (socklen_t) sizeof *cred;
	    return ret;
	  }

	case SO_REUSEADDR:
	  {
	    unsigned int *reuseaddr = (unsigned int *) optval;

	    if (*optlen < (socklen_t) sizeof *reuseaddr)
	      {
		set_errno (EINVAL);
		return -1;
	      }
	    *reuseaddr = saw_reuseaddr();
	    *optlen = (socklen_t) sizeof *reuseaddr;
	    break;
	  }

	case SO_RCVBUF:
	case SO_SNDBUF:
	  if (*optlen < (socklen_t) sizeof (int))
	    {
	      set_errno (EINVAL);
	      return -1;
	    }
	  *(int *) optval = (optname == SO_RCVBUF) ? rmem () : wmem ();
	  break;

	case SO_RCVTIMEO:
	case SO_SNDTIMEO:
	  {
	    struct timeval *time_out = (struct timeval *) optval;

	    if (*optlen < (socklen_t) sizeof *time_out)
	      {
		set_errno (EINVAL);
		return -1;
	      }
	    DWORD ms = (optname == SO_RCVTIMEO) ? rcvtimeo () : sndtimeo ();
	    if (ms == 0 || ms == INFINITE)
	      {
		time_out->tv_sec = 0;
		time_out->tv_usec = 0;
	      }
	    else
	      {
		time_out->tv_sec = ms / MSPERSEC;
		time_out->tv_usec = ((ms % MSPERSEC) * USPERSEC) / MSPERSEC;
	      }
	    *optlen = (socklen_t) sizeof *time_out;
	    break;
	  }

	case SO_TYPE:
	  {
	    unsigned int *type = (unsigned int *) optval;
	    *type = get_socket_type ();
	    *optlen = (socklen_t) sizeof *type;
	    break;
	  }

	/* AF_UNIX sockets simply ignore all other SOL_SOCKET options. */

	case SO_LINGER:
	  {
	    struct linger *linger = (struct linger *) optval;
	    memset (linger, 0, sizeof *linger);
	    *optlen = (socklen_t) sizeof *linger;
	    break;
	  }

	default:
	  {
	    unsigned int *val = (unsigned int *) optval;
	    *val = 0;
	    *optlen = (socklen_t) sizeof *val;
	    break;
	  }
	}
      break;

    default:
      set_errno (ENOPROTOOPT);
      return -1;
    }

  return 0;
}

int
fhandler_socket_unix::ioctl (unsigned int cmd, void *p)
{
  int ret = -1;

  switch (cmd)
    {
    case FIOASYNC:
#ifdef __x86_64__
    case _IOW('f', 125, int):
#endif
      break;
    case FIONREAD:
#ifdef __x86_64__
    case _IOR('f', 127, int):
#endif
    case FIONBIO:
      {
	const bool was_nonblocking = is_nonblocking ();
	set_nonblocking (*(int *) p);
	const bool now_nonblocking = is_nonblocking ();
	if (was_nonblocking != now_nonblocking)
	  set_pipe_non_blocking (now_nonblocking);
	ret = 0;
	break;
      }
    case SIOCATMARK:
      break;
    default:
      ret = fhandler_socket::ioctl (cmd, p);
      break;
    }
  return ret;
}

int
fhandler_socket_unix::fcntl (int cmd, intptr_t arg)
{
  int ret = -1;

  switch (cmd)
    {
    case F_SETOWN:
      break;
    case F_GETOWN:
      break;
    case F_SETFL:
      {
	const bool was_nonblocking = is_nonblocking ();
	const int allowed_flags = O_APPEND | O_NONBLOCK_MASK;
	int new_flags = arg & allowed_flags;
	if ((new_flags & OLD_O_NDELAY) && (new_flags & O_NONBLOCK))
	  new_flags &= ~OLD_O_NDELAY;
	set_flags ((get_flags () & ~allowed_flags) | new_flags);
	const bool now_nonblocking = is_nonblocking ();
	if (was_nonblocking != now_nonblocking)
	  set_pipe_non_blocking (now_nonblocking);
	ret = 0;
	break;
      }
    default:
      ret = fhandler_socket::fcntl (cmd, arg);
      break;
    }
  return ret;
}

int __reg2
fhandler_socket_unix::fstat (struct stat *buf)
{
  int ret = 0;

  if (!get_sun_path ()
      || get_sun_path ()->un_len <= (socklen_t) sizeof (sa_family_t)
      || get_sun_path ()->un.sun_path[0] == '\0')
    return fhandler_socket::fstat (buf);
  ret = fhandler_base::fstat_fs (buf);
  if (!ret)
    {
      buf->st_mode = (buf->st_mode & ~S_IFMT) | S_IFSOCK;
      buf->st_size = 0;
    }
  return ret;
}

int __reg2
fhandler_socket_unix::fstatvfs (struct statvfs *sfs)
{
  if (!get_sun_path ()
      || get_sun_path ()->un_len <= (socklen_t) sizeof (sa_family_t)
      || get_sun_path ()->un.sun_path[0] == '\0')
    return fhandler_socket::fstatvfs (sfs);
  fhandler_disk_file fh (pc);
  fh.get_device () = FH_FS;
  return fh.fstatvfs (sfs);
}

int
fhandler_socket_unix::fchmod (mode_t newmode)
{
  if (!get_sun_path ()
      || get_sun_path ()->un_len <= (socklen_t) sizeof (sa_family_t)
      || get_sun_path ()->un.sun_path[0] == '\0')
    return fhandler_socket::fchmod (newmode);
  fhandler_disk_file fh (pc);
  fh.get_device () = FH_FS;
  /* Kludge: Don't allow to remove read bit on socket files for
     user/group/other, if the accompanying write bit is set.  It would
     be nice to have exact permissions on a socket file, but it's
     necessary that somebody able to access the socket can always read
     the contents of the socket file to avoid spurious "permission
     denied" messages. */
  newmode |= (newmode & (S_IWUSR | S_IWGRP | S_IWOTH)) << 1;
  return fh.fchmod (S_IFSOCK | newmode);
}

int
fhandler_socket_unix::fchown (uid_t uid, gid_t gid)
{
  if (!get_sun_path ()
      || get_sun_path ()->un_len <= (socklen_t) sizeof (sa_family_t)
      || get_sun_path ()->un.sun_path[0] == '\0')
    return fhandler_socket::fchown (uid, gid);
  fhandler_disk_file fh (pc);
  return fh.fchown (uid, gid);
}

int
fhandler_socket_unix::facl (int cmd, int nentries, aclent_t *aclbufp)
{
  if (!get_sun_path ()
      || get_sun_path ()->un_len <= (socklen_t) sizeof (sa_family_t)
      || get_sun_path ()->un.sun_path[0] == '\0')
    return fhandler_socket::facl (cmd, nentries, aclbufp);
  fhandler_disk_file fh (pc);
  return fh.facl (cmd, nentries, aclbufp);
}

int
fhandler_socket_unix::link (const char *newpath)
{
  if (!get_sun_path ()
      || get_sun_path ()->un_len <= (socklen_t) sizeof (sa_family_t)
      || get_sun_path ()->un.sun_path[0] == '\0')
    return fhandler_socket::link (newpath);
  fhandler_disk_file fh (pc);
  return fh.link (newpath);
}
