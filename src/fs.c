/* 2014, Copyright © Intel Coporation, license MIT, see COPYING file */
#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <syslog.h>
#include <limits.h>

#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sendfile.h>

#include "fs.h"

struct explore_dirs
{
  int wanted;
  fs_callback callback;
  struct fs_entry entry;
  char path[PATH_MAX];
};

static enum fs_action _explore_directory_content (struct explore_dirs *ed);

static enum fs_action
_explore_entry (struct explore_dirs *ed, enum fs_type type)
{
  enum fs_action action;

  switch (type)
    {
    case type_regular:
      if (!(ed->wanted & wants_others))
	break;
      ed->entry.type = type_regular;
      return ed->callback (&ed->entry);

    case type_directory_pre:
      if (!(ed->wanted & (wants_directory_pre | wants_directory_post)))
	break;
      if (!(ed->wanted & wants_directory_pre))
	action = _explore_directory_content (ed);
      else
	{
	  ed->entry.type = type_directory_pre;
	  action = ed->callback (&ed->entry);
	  if (action == action_skip_subtree)
	    action = action_continue;
	  else if (action == action_continue)
	    action = _explore_directory_content (ed);
	  else
	    return action;
	}
      if (action != action_continue || !(ed->wanted & wants_directory_post))
	return action;
      ed->entry.type = type_directory_post;
      return ed->callback (&ed->entry);

    case type_symlink:
      if (!(ed->wanted & wants_others))
	break;
      ed->entry.type = type_symlink;
      return ed->callback (&ed->entry);

    case type_others:
      if (!(ed->wanted & wants_others))
	break;
      ed->entry.type = type_others;
      return ed->callback (&ed->entry);

    default:
      errno = EINVAL;
      return action_stop_error;
    }

  if (!(ed->wanted & wants_error_on_unwanted))
    return action_continue;

  errno = ECANCELED;
  return action_stop_error;
}

static enum fs_action
_explore_directory_content (struct explore_dirs *ed)
{
  enum fs_action action;
  const char *savename;
  DIR *dir;
  struct dirent *ent;
  int length;
  int n;
#if !defined(_DIRENT_HAVE_D_TYPE)
  struct stat s;
#endif

  /* check length */
  length = ed->entry.length;
  if (length + 1 >= PATH_MAX)
    {
      errno = ENAMETOOLONG;
      return action_stop_error;
    }

  /* open the directory */
  dir = opendir (ed->path);
  if (!dir)
    return action_stop_error;

  /* save state */
  ed->path[length++] = '/';
  ed->path[length] = 0;
  savename = ed->entry.name;
  ed->entry.name = ed->path + length;

  /* iterate on entries */
  for (;;)
    {
      /* get the next entry */
      errno = 0;
      ent = readdir (dir);
      if (ent == NULL)
	{
	  action = errno ? action_stop_error : action_continue;
	  break;
	}

      /* get the length of the entry */
      n = (int) strlen (ent->d_name);
      assert (n > 0);

      /* skips special files '.' and '..' */
      if (n <= 2 && ent->d_name[0] == '.' && ent->d_name[n - 1] == '.')
	continue;

      /* check path length */
      if (length + n >= PATH_MAX)
	{
	  errno = ENAMETOOLONG;
	  action = action_stop_error;
	  break;
	}

      /* create the entry path */
      memcpy (ed->path + length, ent->d_name, n + 1);

      /* inspect the entry type */
#if defined(_DIRENT_HAVE_D_TYPE)
#define IS_A_(x)  (ent->d_type == DT_##x)
#else
#define IS_A_(x)  (S_IS##x (s.st_mode))
      if (lstat (ed->path, &s))
	{
	  action = action_stop_error;
	  break;
	}
#endif
      action = _explore_entry (ed,
			       IS_A_ (REG) ? type_regular : IS_A_ (DIR) ?
			       type_directory_pre : IS_A_ (LNK) ? type_symlink
			       : type_others);
#undef IS_A_

      /* inspect action */
      if (action == action_stop_ok || action == action_stop_error)
	break;

      if (action == action_skip_siblings)
	{
	  action = action_stop_ok;
	  break;
	}
    }
  /* close the directory */
  closedir (dir);

  /* restore state and exit */
  ed->entry.length = --length;
  ed->path[length] = 0;
  ed->entry.name = savename;
  return action;
}


int
fs_explore (const char *directory, int wanted, fs_callback callback,
	    void *data)
{
  struct explore_dirs ed;

  ed.wanted = wanted;
  ed.callback = callback;
  ed.entry.data = data;
  ed.entry.base = directory;
  ed.entry.path = ed.path;
  ed.entry.length = (int) strlen (directory);
  ed.entry.relpath = ed.entry.path + ed.entry.length;

  /* check length */
  if (ed.entry.length >= PATH_MAX)
    {
      errno = ENAMETOOLONG;
      return -1;
    }

  memcpy (ed.path, directory, ed.entry.length + 1);
  return _explore_directory_content (&ed) == action_stop_error ? -1 : 0;
}

static enum fs_action
_cbfun_remove (const struct fs_entry *entry)
{
  return (entry->type == type_directory_post ? rmdir : unlink) (entry->path);
}

int
fs_remove_directory_content (const char *path)
{
  return fs_explore (path, wants_any_post, _cbfun_remove, NULL);
}

int
fs_remove_directory (const char *path, int force)
{
  if (force && fs_remove_directory_content (path))
    return -1;
  return rmdir (path);
}

int
fs_remove_any (const char *path)
{
  return unlink (path) || errno != EISDIR
    || fs_remove_directory (path, 1) ? -1 : 0;
}

int
fs_copy_file (const char *dest, const char *src, int force)
{
  int fdfrom, fdto, result;
  ssize_t length;
  size_t count;
  struct stat s;

  result = -1;
  fdfrom = open (src, O_RDONLY);
  if (fdfrom >= 0)
    {
      if (!fstat (fdfrom, &s))
	{
	  fdto = open (src, O_WRONLY | O_TRUNC | O_CREAT, s.st_mode | 0200);
	  if (fdto >= 0)
	    {
	      for (;;)
		{
		  count = (~(size_t) 0) >> 1;
		  if ((off_t) count > s.st_size)
		    count = (size_t) s.st_size;
		  length = sendfile (fdto, fdfrom, NULL, count);
		  if (length < 0)
		    break;
		  s.st_size -= (off_t) length;
		  if (s.st_size == 0)
		    {
		      result = 0;
		      break;
		    }
		}
	    }
	  close (fdto);
	  if (result)
	    unlink (dest);
	}
      close (fdfrom);
    }
  return result;
}

static enum fs_action
_cbfun_copy (const struct fs_entry *entry)
{
  return entry->type == type_regular ?
    fs_copy_file (entry->relpath, entry->path, 1) : mkdir (entry->relpath,
							   0700);
}

int
fs_copy_directory (const char *dest, const char *src, int force)
{
  int result;
  char savewd[PATH_MAX];

  if (!getcwd (savewd, sizeof savewd))
    return -1;

  if (chdir (dest))
    return -1;

  result =
    fs_explore (src,
		wants_regular | wants_directory_pre | wants_error_on_unwanted,
		_cbfun_copy, NULL);
  if (chdir (savewd))
    return -1;

  return result;
}
