/*
 * Copyright (C) 2009, 2010 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License (not later!)
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "trace-cmd.h"

static struct event_format *fgraph_ret_event;
static int fgraph_ret_id;
static int long_size;

static int get_field_val(struct trace_seq *s, void *data,
			 struct event_format *event, const char *name,
			 unsigned long long *val)
{
	struct format_field *field;

	field = pevent_find_any_field(event, name);
	if (!field) {
		trace_seq_printf(s, "<CANT FIND FIELD %s>", name);
		return -1;
	}

	if (pevent_read_number_field(field, data, val)) {
		trace_seq_printf(s, " %s=INVALID", name);
		return -1;
	}

	return 0;
}

static int function_handler(struct trace_seq *s, struct record *record,
			    struct event_format *event)
{
	struct pevent *pevent = event->pevent;
	unsigned long long function;
	const char *func;
	void *data = record->data;

	if (get_field_val(s, data, event, "ip", &function))
		return trace_seq_putc(s, '!');

	func = pevent_find_function(pevent, function);
	if (func)
		trace_seq_printf(s, "%s <-- ", func);
	else
		trace_seq_printf(s, "0x%llx", function);

	if (get_field_val(s, data, event, "parent_ip", &function))
		return trace_seq_putc(s, '!');

	func = pevent_find_function(pevent, function);
	if (func)
		trace_seq_printf(s, "%s", func);
	else
		trace_seq_printf(s, "0x%llx", function);

	return 0;
}

#define TRACE_GRAPH_INDENT		2

static struct record *
get_return_for_leaf(struct trace_seq *s, int cpu, int cur_pid,
		    unsigned long long cur_func, struct record *next)
{
	unsigned long long val;
	unsigned long long type;
	unsigned long long pid;

	/* Searching a common field, can use any event */
	if (get_field_val(s, next->data, fgraph_ret_event, "common_type", &type))
		return NULL;

	if (type != fgraph_ret_id)
		return NULL;

	if (get_field_val(s, next->data, fgraph_ret_event, "common_pid", &pid))
		return NULL;

	if (cur_pid != pid)
		return NULL;

	/* We aleady know this is a funcgraph_ret_event */
	if (get_field_val(s, next->data, fgraph_ret_event, "func", &val))
		return NULL;

	if (cur_func != val)
		return NULL;

	/* this is a leaf, now advance the iterator */
	return tracecmd_read_data(tracecmd_curr_thread_handle, cpu);
}

/* Signal a overhead of time execution to the output */
static void print_graph_overhead(struct trace_seq *s,
				 unsigned long long duration)
{
	/* Non nested entry or return */
	if (duration == ~0ULL)
		return (void)trace_seq_printf(s, "  ");

	/* Duration exceeded 100 msecs */
	if (duration > 100000ULL)
		return (void)trace_seq_printf(s, "! ");

	/* Duration exceeded 10 msecs */
	if (duration > 10000ULL)
		return (void)trace_seq_printf(s, "+ ");

	trace_seq_printf(s, "  ");
}

static void print_graph_duration(struct trace_seq *s, unsigned long long duration)
{
	unsigned long usecs = duration / 1000;
	unsigned long nsecs_rem = duration % 1000;
	/* log10(ULONG_MAX) + '\0' */
	char msecs_str[21];
	char nsecs_str[5];
	int len;
	int i;

	sprintf(msecs_str, "%lu", usecs);

	/* Print msecs */
	len = s->len;
	trace_seq_printf(s, "%lu", usecs);

	/* Print nsecs (we don't want to exceed 7 numbers) */
	if ((s->len - len) < 7) {
		snprintf(nsecs_str, MIN(sizeof(nsecs_str), 8 - len), "%03lu", nsecs_rem);
		trace_seq_printf(s, ".%s", nsecs_str);
	}

	len = s->len - len;

	trace_seq_puts(s, " us ");

	/* Print remaining spaces to fit the row's width */
	for (i = len; i < 7; i++)
		trace_seq_putc(s, ' ');

	trace_seq_puts(s, "|  ");
}

static int
print_graph_entry_leaf(struct trace_seq *s,
		       struct event_format *event, void *data, struct record *ret_rec)
{
	struct pevent *pevent = event->pevent;
	unsigned long long rettime, calltime;
	unsigned long long duration, depth;
	unsigned long long val;
	const char *func;
	int i;


	if (get_field_val(s, ret_rec->data, fgraph_ret_event, "rettime", &rettime))
		return trace_seq_putc(s, '!');

	if (get_field_val(s, ret_rec->data, fgraph_ret_event, "calltime", &calltime))
		return trace_seq_putc(s, '!');

	duration = rettime - calltime;

	/* Overhead */
	print_graph_overhead(s, duration);

	/* Duration */
	print_graph_duration(s, duration);

	if (get_field_val(s, data, event, "depth", &depth))
		return trace_seq_putc(s, '!');

	/* Function */
	for (i = 0; i < (int)(depth * TRACE_GRAPH_INDENT); i++)
		trace_seq_putc(s, ' ');

	if (get_field_val(s, data, event, "func", &val))
		return trace_seq_putc(s, '!');
	func = pevent_find_function(pevent, val);

	if (func)
		return trace_seq_printf(s, "%s();", func);
	else
		return trace_seq_printf(s, "%llx();", val);
}

static int print_graph_nested(struct trace_seq *s,
			      struct event_format *event, void *data)
{
	struct pevent *pevent = event->pevent;
	unsigned long long depth;
	unsigned long long val;
	const char *func;
	int i;

	/* No overhead */
	print_graph_overhead(s, -1);

	/* No time */
	trace_seq_puts(s, "           |  ");

	if (get_field_val(s, data, event, "depth", &depth))
		return trace_seq_putc(s, '!');

	/* Function */
	for (i = 0; i < (int)(depth * TRACE_GRAPH_INDENT); i++)
		trace_seq_putc(s, ' ');

	if (get_field_val(s, data, event, "func", &val))
		return trace_seq_putc(s, '!');

	func = pevent_find_function(pevent, val);

	if (func)
		return trace_seq_printf(s, "%s() {", func);
	else
		return trace_seq_printf(s, "%llx() {", val);
}

static int
fgraph_ent_handler(struct trace_seq *s, struct record *record,
		   struct event_format *event)
{
	struct record *rec;
	unsigned long long val, pid;
	void *data = record->data;
	int cpu = record->cpu;

	if (get_field_val(s, data, event, "common_pid", &pid))
		return trace_seq_putc(s, '!');

	if (get_field_val(s, data, event, "func", &val))
		return trace_seq_putc(s, '!');

	rec = tracecmd_peek_data(tracecmd_curr_thread_handle, cpu);
	if (rec)
		rec = get_return_for_leaf(s, cpu, pid, val, rec);

	if (rec) {
		/*
		 * If this is a leaf function, then get_return_for_leaf
		 * returns the return of the function
		 */
		print_graph_entry_leaf(s, event, data, rec);
		free_record(rec);
	} else
		print_graph_nested(s, event, data);

	return 0;
}

static int
fgraph_ret_handler(struct trace_seq *s, struct record *record,
		   struct event_format *event)
{
	unsigned long long rettime, calltime;
	unsigned long long duration, depth;
	void *data = record->data;
	int i;

	if (get_field_val(s, data, event, "rettime", &rettime))
		return trace_seq_putc(s, '!');

	if (get_field_val(s, data, event, "calltime", &calltime))
		return trace_seq_putc(s, '!');

	duration = rettime - calltime;

	/* Overhead */
	print_graph_overhead(s, duration);

	/* Duration */
	print_graph_duration(s, duration);

	if (get_field_val(s, data, event, "depth", &depth))
		return trace_seq_putc(s, '!');

	/* Function */
	for (i = 0; i < (int)(depth * TRACE_GRAPH_INDENT); i++)
		trace_seq_putc(s, ' ');

	trace_seq_putc(s, '}');

	return 0;
}

static int
trace_stack_handler(struct trace_seq *s, struct record *record,
		    struct event_format *event)
{
	struct format_field *field;
	unsigned long long addr;
	const char *func;
	void *data = record->data;
	int i;

	field = pevent_find_any_field(event, "caller");
	if (!field) {
		trace_seq_printf(s, "<CANT FIND FIELD %s>", "caller");
		return 0;
	}

	trace_seq_puts(s, "<stack trace>\n");

	for (i = 0; i < field->size; i += long_size) {
		addr = pevent_read_number(event->pevent,
					  data + field->offset + i, long_size);

		if ((long_size == 8 && addr == (unsigned long long)-1) ||
		    ((int)addr == -1))
			break;

		func = pevent_find_function(event->pevent, addr);
		if (func)
			trace_seq_printf(s, "=> %s (%llx)\n", func, addr);
		else
			trace_seq_printf(s, "=> %llx\n", addr);
	}

	return 0;
}

int tracecmd_ftrace_overrides(struct tracecmd_input *handle)
{
	struct pevent *pevent;
	struct event_format *event;

	pevent = tracecmd_get_pevent(handle);

	pevent_register_event_handler(pevent, -1, "ftrace", "function",
				      function_handler);

	pevent_register_event_handler(pevent, -1, "ftrace", "funcgraph_entry",
				      fgraph_ent_handler);

	pevent_register_event_handler(pevent, -1, "ftrace", "funcgraph_exit",
				      fgraph_ret_handler);

	pevent_register_event_handler(pevent, -1, "ftrace", "kernel_stack",
				      trace_stack_handler);

	/* Store the func ret id and event for later use */
	event = pevent_find_event_by_name(pevent, "ftrace", "funcgraph_exit");
	if (!event)
		return 0;

	long_size = tracecmd_long_size(handle);

	fgraph_ret_id = event->id;
	fgraph_ret_event = event;

	return 0;
}
