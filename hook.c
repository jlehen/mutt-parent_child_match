/* 
 * Copyright (C) 1996-2002,2004,2007 Michael R. Elkins <me@mutt.org>, and others
 *
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software
 *     Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include "mutt.h"
#include "mailbox.h"
#include "mutt_crypt.h"

#ifdef USE_COMPRESSED
#include "compress.h"
#endif

#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>

typedef struct hook
{
  int type;		/* hook type */
  REGEXP rx;		/* regular expression */
  char *command;	/* filename, command or pattern to execute */
  pattern_t *pattern;	/* used for fcc,save,send-hook */
  struct hook *next;
} HOOK;

static HOOK *Hooks = NULL;

static int current_hook_type = 0;

int mutt_parse_hook (BUFFER *buf, BUFFER *s, unsigned long data, BUFFER *err)
{
  HOOK *ptr;
  BUFFER command, pattern;
  int rc, not = 0;
  regex_t *rx = NULL;
  pattern_t *pat = NULL;
  char path[_POSIX_PATH_MAX];

  mutt_buffer_init (&pattern);
  mutt_buffer_init (&command);

  if (*s->dptr == '!')
  {
    s->dptr++;
    SKIPWS (s->dptr);
    not = 1;
  }

  mutt_extract_token (&pattern, s, 0);

  if (!MoreArgs (s))
  {
    strfcpy (err->data, _("too few arguments"), err->dsize);
    goto error;
  }

  mutt_extract_token (&command, s, (data & (MUTT_FOLDERHOOK | MUTT_SENDHOOK | MUTT_SEND2HOOK | MUTT_ACCOUNTHOOK | MUTT_REPLYHOOK)) ?  MUTT_TOKEN_SPACE : 0);

  if (!command.data)
  {
    strfcpy (err->data, _("too few arguments"), err->dsize);
    goto error;
  }

  if (MoreArgs (s))
  {
    strfcpy (err->data, _("too many arguments"), err->dsize);
    goto error;
  }

  if (data & (MUTT_FOLDERHOOK | MUTT_MBOXHOOK))
  {
    /* Accidentally using the ^ mailbox shortcut in the .muttrc is a
     * common mistake */
    if ((*pattern.data == '^') && (! CurrentFolder))
    {
      strfcpy (err->data, _("current mailbox shortcut '^' is unset"), err->dsize);
      goto error;
    }

    strfcpy (path, pattern.data, sizeof (path));
    _mutt_expand_path (path, sizeof (path), 1);

    /* Check for other mailbox shortcuts that expand to the empty string.
     * This is likely a mistake too */
    if (!*path && *pattern.data)
    {
      strfcpy (err->data, _("mailbox shortcut expanded to empty regexp"), err->dsize);
      goto error;
    }

    FREE (&pattern.data);
    memset (&pattern, 0, sizeof (pattern));
    pattern.data = safe_strdup (path);
  }
#ifdef USE_COMPRESSED
  else if (data & (MUTT_APPENDHOOK | MUTT_OPENHOOK | MUTT_CLOSEHOOK)) {
    if (mutt_comp_valid_command (command.data) == 0) {
      strfcpy (err->data, _("badly formatted command string"), err->dsize);
      return -1;
    }
  }
#endif
  else if (DefaultHook && !(data & (MUTT_CHARSETHOOK | MUTT_ICONVHOOK | MUTT_ACCOUNTHOOK))
           && (!WithCrypto || !(data & MUTT_CRYPTHOOK))
      )
  {
    char tmp[HUGE_STRING];

    /* At this stage remain only message-hooks, reply-hooks, send-hooks,
     * send2-hooks, save-hooks, and fcc-hooks: All those allowing full
     * patterns. If given a simple regexp, we expand $default_hook.
     */
    strfcpy (tmp, pattern.data, sizeof (tmp));
    mutt_check_simple (tmp, sizeof (tmp), DefaultHook);
    FREE (&pattern.data);
    memset (&pattern, 0, sizeof (pattern));
    pattern.data = safe_strdup (tmp);
  }

  if (data & (MUTT_MBOXHOOK | MUTT_SAVEHOOK | MUTT_FCCHOOK))
  {
    strfcpy (path, command.data, sizeof (path));
    mutt_expand_path (path, sizeof (path));
    FREE (&command.data);
    memset (&command, 0, sizeof (command));
    command.data = safe_strdup (path);
  }

  /* check to make sure that a matching hook doesn't already exist */
  for (ptr = Hooks; ptr; ptr = ptr->next)
  {
    if (ptr->type == data &&
	ptr->rx.not == not &&
	!mutt_strcmp (pattern.data, ptr->rx.pattern))
    {
      if (data & (MUTT_FOLDERHOOK | MUTT_SENDHOOK | MUTT_SEND2HOOK | MUTT_MESSAGEHOOK | MUTT_ACCOUNTHOOK | MUTT_REPLYHOOK | MUTT_CRYPTHOOK))
      {
	/* these hooks allow multiple commands with the same
	 * pattern, so if we've already seen this pattern/command pair, just
	 * ignore it instead of creating a duplicate */
	if (!mutt_strcmp (ptr->command, command.data))
	{
	  FREE (&command.data);
	  FREE (&pattern.data);
	  return 0;
	}
      }
      else
      {
	/* other hooks only allow one command per pattern, so update the
	 * entry with the new command.  this currently does not change the
	 * order of execution of the hooks, which i think is desirable since
	 * a common action to perform is to change the default (.) entry
	 * based upon some other information. */
	FREE (&ptr->command);
	ptr->command = command.data;
	FREE (&pattern.data);
	return 0;
      }
    }
    if (!ptr->next)
      break;
  }

  if (data & (MUTT_SENDHOOK | MUTT_SEND2HOOK | MUTT_SAVEHOOK | MUTT_FCCHOOK | MUTT_MESSAGEHOOK | MUTT_REPLYHOOK))
  {
    if ((pat = mutt_pattern_comp (pattern.data,
	   (data & (MUTT_SENDHOOK | MUTT_SEND2HOOK | MUTT_FCCHOOK)) ? 0 : MUTT_FULL_MSG,
				  err)) == NULL)
      goto error;
  }
  else
  {
    /* Hooks not allowing full patterns: Check syntax of regexp */
    rx = safe_malloc (sizeof (regex_t));
#ifdef MUTT_CRYPTHOOK
    if ((rc = REGCOMP (rx, NONULL(pattern.data), ((data & (MUTT_CRYPTHOOK|MUTT_CHARSETHOOK|MUTT_ICONVHOOK)) ? REG_ICASE : 0))) != 0)
#else
    if ((rc = REGCOMP (rx, NONULL(pattern.data), (data & (MUTT_CHARSETHOOK|MUTT_ICONVHOOK)) ? REG_ICASE : 0)) != 0)
#endif /* MUTT_CRYPTHOOK */
    {
      regerror (rc, rx, err->data, err->dsize);
      FREE (&rx);
      goto error;
    }
  }

  if (ptr)
  {
    ptr->next = safe_calloc (1, sizeof (HOOK));
    ptr = ptr->next;
  }
  else
    Hooks = ptr = safe_calloc (1, sizeof (HOOK));
  ptr->type = data;
  ptr->command = command.data;
  ptr->pattern = pat;
  ptr->rx.pattern = pattern.data;
  ptr->rx.rx = rx;
  ptr->rx.not = not;
  return 0;

error:
  FREE (&pattern.data);
  FREE (&command.data);
  return (-1);
}

static void delete_hook (HOOK *h)
{
  FREE (&h->command);
  FREE (&h->rx.pattern);
  if (h->rx.rx)
  {
    regfree (h->rx.rx);
  }
  mutt_pattern_free (&h->pattern);
  FREE (&h);
}

/* Deletes all hooks of type ``type'', or all defined hooks if ``type'' is 0 */
static void delete_hooks (int type)
{
  HOOK *h;
  HOOK *prev;

  while (h = Hooks, h && (type == 0 || type == h->type))
  {
    Hooks = h->next;
    delete_hook (h);
  }

  prev = h; /* Unused assignment to avoid compiler warnings */

  while (h)
  {
    if (type == h->type)
    {
      prev->next = h->next;
      delete_hook (h);
    }
    else
      prev = h;
    h = prev->next;
  }
}

int mutt_parse_unhook (BUFFER *buf, BUFFER *s, unsigned long data, BUFFER *err)
{
  while (MoreArgs (s))
  {
    mutt_extract_token (buf, s, 0);
    if (mutt_strcmp ("*", buf->data) == 0)
    {
      if (current_hook_type)
      {
	snprintf (err->data, err->dsize,
		  _("unhook: Can't do unhook * from within a hook."));
	return -1;
      }
      delete_hooks (0);
    }
    else
    {
      int type = mutt_get_hook_type (buf->data);

      if (!type)
      {
	snprintf (err->data, err->dsize,
		 _("unhook: unknown hook type: %s"), buf->data);
	return (-1);
      }
      if (current_hook_type == type)
      {
	snprintf (err->data, err->dsize,
		  _("unhook: Can't delete a %s from within a %s."),
		  buf->data, buf->data);
	return -1;
      }
      delete_hooks (type);
    }
  }
  return 0;
}

void mutt_folder_hook (char *path)
{
  HOOK *tmp = Hooks;
  BUFFER err, token;

  current_hook_type = MUTT_FOLDERHOOK;

  mutt_buffer_init (&err);
  err.dsize = STRING;
  err.data = safe_malloc (err.dsize);
  mutt_buffer_init (&token);
  for (; tmp; tmp = tmp->next)
  {
    if(!tmp->command)
      continue;

    if (tmp->type & MUTT_FOLDERHOOK)
    {
      if ((regexec (tmp->rx.rx, path, 0, NULL, 0) == 0) ^ tmp->rx.not)
      {
	if (mutt_parse_rc_line (tmp->command, &token, &err) == -1)
	{
	  mutt_error ("%s", err.data);
	  FREE (&token.data);
	  mutt_sleep (1);	/* pause a moment to let the user see the error */
	  current_hook_type = 0;
	  FREE (&err.data);

	  return;
	}
      }
    }
  }
  FREE (&token.data);
  FREE (&err.data);
  
  current_hook_type = 0;
}

char *mutt_find_hook (int type, const char *pat)
{
  HOOK *tmp = Hooks;

  for (; tmp; tmp = tmp->next)
    if (tmp->type & type)
    {
      if (regexec (tmp->rx.rx, pat, 0, NULL, 0) == 0)
	return (tmp->command);
    }
  return (NULL);
}

void mutt_message_hook (CONTEXT *ctx, HEADER *hdr, int type)
{
  BUFFER err, token;
  HOOK *hook;
  pattern_cache_t cache;

  current_hook_type = type;

  mutt_buffer_init (&err);
  err.dsize = STRING;
  err.data = safe_malloc (err.dsize);
  mutt_buffer_init (&token);
  memset (&cache, 0, sizeof (cache));
  for (hook = Hooks; hook; hook = hook->next)
  {
    if(!hook->command)
      continue;

    if (hook->type & type)
      if ((mutt_pattern_exec (hook->pattern, 0, ctx, hdr, &cache) > 0) ^ hook->rx.not)
      {
	if (mutt_parse_rc_line (hook->command, &token, &err) != 0)
	{
	  FREE (&token.data);
	  mutt_error ("%s", err.data);
	  mutt_sleep (1);
	  current_hook_type = 0;
	  FREE (&err.data);

	  return;
	}
        /* Executing arbitrary commands could affect the pattern results,
         * so the cache has to be wiped */
        memset (&cache, 0, sizeof (cache));
      }
  }
  FREE (&token.data);
  FREE (&err.data);

  current_hook_type = 0;
}

static int
mutt_addr_hook (char *path, size_t pathlen, int type, CONTEXT *ctx, HEADER *hdr)
{
  HOOK *hook;
  pattern_cache_t cache;

  memset (&cache, 0, sizeof (cache));
  /* determine if a matching hook exists */
  for (hook = Hooks; hook; hook = hook->next)
  {
    if(!hook->command)
      continue;

    if (hook->type & type)
      if ((mutt_pattern_exec (hook->pattern, 0, ctx, hdr, &cache) > 0) ^ hook->rx.not)
      {
	mutt_make_string (path, pathlen, hook->command, ctx, hdr);
	return 0;
      }
  }

  return -1;
}

void mutt_default_save (char *path, size_t pathlen, HEADER *hdr)
{
  *path = 0;
  if (mutt_addr_hook (path, pathlen, MUTT_SAVEHOOK, Context, hdr) != 0)
  {
    char tmp[_POSIX_PATH_MAX];
    ADDRESS *adr;
    ENVELOPE *env = hdr->env;
    int fromMe = mutt_addr_is_user (env->from);

    if (!fromMe && env->reply_to && env->reply_to->mailbox)
      adr = env->reply_to;
    else if (!fromMe && env->from && env->from->mailbox)
      adr = env->from;
    else if (env->to && env->to->mailbox)
      adr = env->to;
    else if (env->cc && env->cc->mailbox)
      adr = env->cc;
    else
      adr = NULL;
    if (adr)
    {
      mutt_safe_path (tmp, sizeof (tmp), adr);
      snprintf (path, pathlen, "=%s", tmp);
    }
  }
}

void mutt_select_fcc (char *path, size_t pathlen, HEADER *hdr)
{
  ADDRESS *adr;
  char buf[_POSIX_PATH_MAX];
  ENVELOPE *env = hdr->env;

  if (mutt_addr_hook (path, pathlen, MUTT_FCCHOOK, NULL, hdr) != 0)
  {
    if ((option (OPTSAVENAME) || option (OPTFORCENAME)) &&
	(env->to || env->cc || env->bcc))
    {
      adr = env->to ? env->to : (env->cc ? env->cc : env->bcc);
      mutt_safe_path (buf, sizeof (buf), adr);
      mutt_concat_path (path, NONULL(Maildir), buf, pathlen);
      if (!option (OPTFORCENAME) && mx_access (path, W_OK) != 0)
	strfcpy (path, NONULL (Outbox), pathlen);
    }
    else
      strfcpy (path, NONULL (Outbox), pathlen);
  }
  mutt_pretty_mailbox (path, pathlen);
}

static char *_mutt_string_hook (const char *match, int hook)
{
  HOOK *tmp = Hooks;

  for (; tmp; tmp = tmp->next)
  {
    if ((tmp->type & hook) && ((match &&
	 regexec (tmp->rx.rx, match, 0, NULL, 0) == 0) ^ tmp->rx.not))
      return (tmp->command);
  }
  return (NULL);
}

static LIST *_mutt_list_hook (const char *match, int hook)
{
  HOOK *tmp = Hooks;
  LIST *matches = NULL;

  for (; tmp; tmp = tmp->next)
  {
    if ((tmp->type & hook) &&
        ((match && regexec (tmp->rx.rx, match, 0, NULL, 0) == 0) ^ tmp->rx.not))
      matches = mutt_add_list (matches, tmp->command);
  }
  return (matches);
}

char *mutt_charset_hook (const char *chs)
{
  return _mutt_string_hook (chs, MUTT_CHARSETHOOK);
}

char *mutt_iconv_hook (const char *chs)
{
  return _mutt_string_hook (chs, MUTT_ICONVHOOK);
}

LIST *mutt_crypt_hook (ADDRESS *adr)
{
  return _mutt_list_hook (adr->mailbox, MUTT_CRYPTHOOK);
}

#ifdef USE_SOCKET
void mutt_account_hook (const char* url)
{
  /* parsing commands with URLs in an account hook can cause a recursive
   * call. We just skip processing if this occurs. Typically such commands
   * belong in a folder-hook -- perhaps we should warn the user. */
  static int inhook = 0;

  HOOK* hook;
  BUFFER token;
  BUFFER err;

  if (inhook)
    return;

  mutt_buffer_init (&err);
  err.dsize = STRING;
  err.data = safe_malloc (err.dsize);
  mutt_buffer_init (&token);

  for (hook = Hooks; hook; hook = hook->next)
  {
    if (! (hook->command && (hook->type & MUTT_ACCOUNTHOOK)))
      continue;

    if ((regexec (hook->rx.rx, url, 0, NULL, 0) == 0) ^ hook->rx.not)
    {
      inhook = 1;

      if (mutt_parse_rc_line (hook->command, &token, &err) == -1)
      {
	FREE (&token.data);
	mutt_error ("%s", err.data);
	FREE (&err.data);
	mutt_sleep (1);

        inhook = 0;
	return;
      }

      inhook = 0;
    }
  }

  FREE (&token.data);
  FREE (&err.data);
}
#endif
