/* Copyright (C) 2002 Timo Sirainen */

/* Implementated against draft-ietf-imapext-sort-10 and
   draft-ietf-imapext-thread-12 */

#include "lib.h"
#include "buffer.h"
#include "charset-utf8.h"
#include "message-header-decode.h"
#include "imap-base-subject.h"

static int header_decode(const unsigned char *data, size_t size,
			 const char *charset, void *context)
{
	buffer_t *buf = context;
        struct charset_translation *t;
	unsigned char *buf_data;
	size_t pos, used_size;

	pos = buffer_get_used_size(buf);
	if (charset == NULL) {
		/* It's ASCII. */
		buffer_append(buf, data, size);
	} else {
		t = charset_to_utf8_begin(charset, NULL);
		if (t != NULL) {
			(void)charset_to_ucase_utf8(t, data, &size, buf);
                        charset_to_utf8_end(t);
		}
	}

	if (size > 0) {
		/* @UNSAFE: uppercase it. Current draft specifies that we
		   should touch only ASCII. */
		buf_data = buffer_get_modifyable_data(buf, &used_size);
		for (; pos < used_size; pos++) {
			if (buf_data[pos] >= 'a' && buf_data[pos] <= 'z')
				buf_data[pos] = buf_data[pos] - 'a' + 'A';
		}
	}

	return TRUE;
}

static void pack_whitespace(buffer_t *buf)
{
	char *data, *dest;
	int last_lwsp;

	data = buffer_get_modifyable_data(buf, NULL);

	/* check if we need to do anything */
	while (*data != '\0') {
		if (*data == '\t' || *data == '\n' || *data == '\r' ||
		    (*data == ' ' && (data[1] == ' ' || data[1] == '\t')))
			break;
		data++;
	}

	if (*data == '\0')
		return;

	/* @UNSAFE: convert/pack the whitespace */
	dest = data; last_lwsp = FALSE;
	while (*data != '\0') {
		if (*data == '\t' || *data == ' ' ||
		    *data == '\r' || *data == '\n') {
			if (!last_lwsp) {
				*dest++ = ' ';
				last_lwsp = TRUE;
			}
		} else {
			*dest++ = *data;
			last_lwsp = FALSE;
		}
		data++;
	}
	*dest = '\0';

	data = buffer_get_modifyable_data(buf, NULL);
	buffer_set_used_size(buf, (size_t) (dest - data)+1);
}

static void remove_subj_trailers(buffer_t *buf, int *is_reply_or_forward)
{
	const char *data;
	size_t orig_size, size;

	/* subj-trailer    = "(fwd)" / WSP */
	data = buffer_get_data(buf, &orig_size);

	if (orig_size < 2) /* size includes trailing \0 */
		return;

	for (size = orig_size-2; size > 0; ) {
		if (data[size] == ' ')
			size--;
		else if (size >= 5 &&
			 memcmp(data + size - 5, "(fwd)", 5) == 0) {
			if (is_reply_or_forward != NULL)
				*is_reply_or_forward = TRUE;
			size -= 5;
		} else {
			break;
		}
	}

	if (size != orig_size-2) {
		buffer_set_used_size(buf, size);
		buffer_append_c(buf, '\0');
	}
}

static int remove_blob(const char **datap)
{
	const char *data = *datap;

	if (*data != '[')
		return FALSE;

	data++;
	while (*data != '\0' && *data != '[' && *data != ']')
		data++;

	if (*data != ']')
		return FALSE;

	data++;
	if (*data == ' ')
		data++;

	*datap = data;
	return TRUE;
}

static int remove_subj_leader(buffer_t *buf, int *is_reply_or_forward)
{
	const char *data, *orig_data;
	int ret = FALSE;

	/* subj-leader     = (*subj-blob subj-refwd) / WSP

	   subj-blob       = "[" *BLOBCHAR "]" *WSP
	   subj-refwd      = ("re" / ("fw" ["d"])) *WSP [subj-blob] ":"

	   BLOBCHAR        = %x01-5a / %x5c / %x5e-7f
	                   ; any CHAR except '[' and ']' */
	orig_data = data = buffer_get_data(buf, NULL);

	if (*data == ' ') {
		/* independent from checks below - always removed */
		data++;
		buffer_set_start_pos(buf, buffer_get_start_pos(buf)+1);
		ret = TRUE;
	}

	while (*data == '[') {
		if (!remove_blob(&data))
			return ret;
	}

	if (strncasecmp(data, "re", 2) == 0)
		data += 2;
	else if (strncasecmp(data, "fwd", 3) == 0)
		data += 3;
	else if (strncasecmp(data, "fw", 2) == 0)
		data += 2;
	else
		return ret;

	if (*data == ' ')
		data++;

	if (*data == '[' && !remove_blob(&data))
		return ret;

	if (*data != ':')
		return ret;

	data++;
	buffer_set_start_pos(buf, buffer_get_start_pos(buf) +
			     (size_t) (data - orig_data));
	if (is_reply_or_forward != NULL)
		*is_reply_or_forward = TRUE;
	return TRUE;
}

static int remove_blob_when_nonempty(buffer_t *buf)
{
	const char *data, *orig_data;

	orig_data = data = buffer_get_data(buf, NULL);
	if (*data == '[' && remove_blob(&data) && *data != '\0') {
		buffer_set_start_pos(buf, buffer_get_start_pos(buf) +
				     (size_t) (data - orig_data));
		return TRUE;
	}

	return FALSE;
}

static int remove_subj_fwd_hdr(buffer_t *buf, int *is_reply_or_forward)
{
	const char *data;
	size_t size;

	/* subj-fwd        = subj-fwd-hdr subject subj-fwd-trl
	   subj-fwd-hdr    = "[fwd:"
	   subj-fwd-trl    = "]" */
	data = buffer_get_data(buf, &size);

	if (strncasecmp(data, "[fwd:", 5) != 0)
		return FALSE;

	if (data[size-2] != ']')
		return FALSE;

	if (is_reply_or_forward != NULL)
		*is_reply_or_forward = TRUE;

	buffer_set_used_size(buf, size-2);
	buffer_append_c(buf, '\0');

	buffer_set_start_pos(buf, buffer_get_start_pos(buf) + 5);
	return TRUE;
}

const char *imap_get_base_subject_cased(pool_t pool, const char *subject,
					int *is_reply_or_forward)
{
	buffer_t *buf;
	size_t subject_len;
	int found;

	if (is_reply_or_forward != NULL)
		*is_reply_or_forward = FALSE;

	subject_len = strlen(subject);
	buf = buffer_create_dynamic(pool, subject_len, (size_t)-1);

	/* (1) Convert any RFC 2047 encoded-words in the subject to
	   UTF-8.  Convert all tabs and continuations to space.
	   Convert all multiple spaces to a single space. */
	message_header_decode((const unsigned char *) subject, subject_len,
			      header_decode, buf);
	buffer_append_c(buf, '\0');

	pack_whitespace(buf);

	do {
		/* (2) Remove all trailing text of the subject that matches
		   the subj-trailer ABNF, repeat until no more matches are
		   possible. */
		remove_subj_trailers(buf, is_reply_or_forward);

		do {
			/* (3) Remove all prefix text of the subject that
			   matches the subj-leader ABNF. */
			found = remove_subj_leader(buf, is_reply_or_forward);

			/* (4) If there is prefix text of the subject that
			   matches the subj-blob ABNF, and removing that prefix
			   leaves a non-empty subj-base, then remove the prefix
			   text. */
			found = remove_blob_when_nonempty(buf) || found;

			/* (5) Repeat (3) and (4) until no matches remain. */
		} while (found);

		/* (6) If the resulting text begins with the subj-fwd-hdr ABNF
		   and ends with the subj-fwd-trl ABNF, remove the
		   subj-fwd-hdr and subj-fwd-trl and repeat from step (2). */
	} while (remove_subj_fwd_hdr(buf, is_reply_or_forward));

	/* (7) The resulting text is the "base subject" used in the
	   SORT. */
	return buffer_get_data(buf, NULL);
}
