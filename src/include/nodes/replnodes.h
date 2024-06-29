/*-------------------------------------------------------------------------
 *
 * replnodes.h
 *	  definitions for replication grammar parse nodes
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/replnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REPLNODES_H
#define REPLNODES_H

#include "access/xlogdefs.h"
#include "nodes/pg_list.h"

typedef enum ReplicationKind /// 复制的类型，只有物理复制和逻辑复制两种
{
	REPLICATION_KIND_PHYSICAL,
	REPLICATION_KIND_LOGICAL
} ReplicationKind;


/* ----------------------
 *		IDENTIFY_SYSTEM command
 * ----------------------
 */
typedef struct IdentifySystemCmd
{
	NodeTag		type;
} IdentifySystemCmd;


/* ----------------------
 *		BASE_BACKUP command
 * ----------------------
 */
typedef struct BaseBackupCmd
{
	NodeTag		type;
	List	   *options;
} BaseBackupCmd;


/* ----------------------
 *		CREATE_REPLICATION_SLOT command
 * ----------------------
 */
typedef struct CreateReplicationSlotCmd
{
	NodeTag		type;
	char	   *slotname;
	ReplicationKind kind;
	char	   *plugin;
	bool		temporary;
	List	   *options;
} CreateReplicationSlotCmd;


/* ----------------------
 *		DROP_REPLICATION_SLOT command
 * ----------------------
 */
typedef struct DropReplicationSlotCmd
{
	NodeTag		type;
	char	   *slotname;
	bool		wait;
} DropReplicationSlotCmd;


/* ----------------------
 *		START_REPLICATION command
 * ----------------------
 */
typedef struct StartReplicationCmd
{
	NodeTag		type;
	ReplicationKind kind;  // 复制类型：物理复制，逻辑复制？
	char	   *slotname;  // 复制槽的名称
	TimeLineID	timeline;  // 时间线
	XLogRecPtr	startpoint;  // 从哪个LSN开始？
	List	   *options;   // 一系列的参数
} StartReplicationCmd;


/* ----------------------
 *		READ_REPLICATION_SLOT command
 * ----------------------
 */
typedef struct ReadReplicationSlotCmd
{
	NodeTag		type;
	char	   *slotname;
} ReadReplicationSlotCmd;


/* ----------------------
 *		TIMELINE_HISTORY command
 * ----------------------
 */
typedef struct TimeLineHistoryCmd
{
	NodeTag		type;
	TimeLineID	timeline;
} TimeLineHistoryCmd;

#endif							/* REPLNODES_H */
