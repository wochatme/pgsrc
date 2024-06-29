/*-------------------------------------------------------------------------
 *
 * timeline.c
 *	  timeline-related functions.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "access/timeline.h"
#include "access/xlog_internal.h"
#include "pg_rewind.h"

/*
 * This is copy-pasted from the backend readTimeLineHistory, modified to
 * return a malloc'd array and to work without backend functions.
 */
/*
 * Try to read a timeline's history file.
 *
 * If successful, return the list of component TLIs (the given TLI followed by
 * its ancestor TLIs).  If we can't find the history file, assume that the
 * timeline has no parents, and return a list of just the specified timeline
 * ID.
 */
TimeLineHistoryEntry * // buffer里面就包含时间线的信息，以0结尾
rewind_parseTimeLineHistory(char *buffer, TimeLineID targetTLI, int *nentries) // 这个函数解析时间线历史文件，有多少次切换，分配的数组就有多少个元素
{
	char	   *fline;
	TimeLineHistoryEntry *entry;
	TimeLineHistoryEntry *entries = NULL;
	int			nlines = 0;
	TimeLineID	lasttli = 0;
	XLogRecPtr	prevend;
	char	   *bufptr;
	bool		lastline = false;

	/*
	 * Parse the file...
	 */
	prevend = InvalidXLogRecPtr;
	bufptr = buffer;
	while (!lastline) // 如果没有读到最后一行
	{
		char	   *ptr;
		TimeLineID	tli;
		uint32		switchpoint_hi;
		uint32		switchpoint_lo;
		int			nfields;

		fline = bufptr;
		while (*bufptr && *bufptr != '\n') // 寻找回车符号
			bufptr++;
		if (!(*bufptr)) // 读到最后一个0，表示这是最后一行的
			lastline = true;
		else
			*bufptr++ = '\0'; // 现在fline就指向了一个以0结尾的字符串，这是一行

		/* skip leading whitespace and check for # comment */
		for (ptr = fline; *ptr; ptr++)
		{
			if (!isspace((unsigned char) *ptr)) // 遇到第一个非空格的字符就跳出扫描
				break;
		}
		if (*ptr == '\0' || *ptr == '#') 到行尾了，这一行没啥内容，就跳过
			continue;

		nfields = sscanf(fline, "%u\t%X/%X", &tli, &switchpoint_hi, &switchpoint_lo); // 这里可以参考时间线历史文件的格式去理解
		// 正常情况下，会读取3列，时间线，LSN的高4个字节，LSN的低4个字节

		if (nfields < 1)
		{
			/* expect a numeric timeline ID as first field of line */
			pg_log_error("syntax error in history file: %s", fline);
			pg_log_error_detail("Expected a numeric timeline ID.");
			exit(1);
		}
		if (nfields != 3)
		{
			pg_log_error("syntax error in history file: %s", fline);
			pg_log_error_detail("Expected a write-ahead log switchpoint location.");
			exit(1);
		}
		if (entries && tli <= lasttli)
		{
			pg_log_error("invalid data in history file: %s", fline);
			pg_log_error_detail("Timeline IDs must be in increasing sequence.");
			exit(1);
		}

		lasttli = tli; // 记录着最后一个时间线

		nlines++; // 成功读到第一行切换点，这个值就是1
		entries = pg_realloc(entries, nlines * sizeof(TimeLineHistoryEntry));

		entry = &entries[nlines - 1];
		entry->tli = tli;
		entry->begin = prevend;
		entry->end = ((uint64) (switchpoint_hi)) << 32 | (uint64) switchpoint_lo;
		prevend = entry->end;

		/* we ignore the remainder of each line */
	}

	if (entries && targetTLI <= lasttli) // 6.history中最后一个时间线是5
	{
		pg_log_error("invalid data in history file");
		pg_log_error_detail("Timeline IDs must be less than child timeline's ID.");
		exit(1);
	}

	/*
	 * Create one more entry for the "tip" of the timeline, which has no entry
	 * in the history file.
	 */
	nlines++;
	if (entries)
		entries = pg_realloc(entries, nlines * sizeof(TimeLineHistoryEntry));
	else
		entries = pg_malloc(1 * sizeof(TimeLineHistoryEntry));

	entry = &entries[nlines - 1];
	entry->tli = targetTLI;
	entry->begin = prevend;
	entry->end = InvalidXLogRecPtr;

	*nentries = nlines;
	return entries;
}
