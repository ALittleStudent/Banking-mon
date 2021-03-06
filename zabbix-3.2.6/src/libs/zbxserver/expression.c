/*
** Zabbix
** Copyright (C) 2001-2017 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "common.h"
#include "zbxserver.h"
#include "evalfunc.h"
#include "db.h"
#include "log.h"
#include "zbxalgo.h"
#include "valuecache.h"
#include "macrofunc.h"

/* The following definitions are used to identify the request field */
/* for various value getters grouped by their scope:                */

/* DBget_item_value(), get_interface_value() */
#define ZBX_REQUEST_HOST_IP		1
#define ZBX_REQUEST_HOST_DNS		2
#define ZBX_REQUEST_HOST_CONN		3
#define ZBX_REQUEST_HOST_PORT		4

/* DBget_item_value() */
#define ZBX_REQUEST_HOST_ID		101
#define ZBX_REQUEST_HOST_HOST		102
#define ZBX_REQUEST_HOST_NAME		103
#define ZBX_REQUEST_HOST_DESCRIPTION	104
#define ZBX_REQUEST_ITEM_ID		105
#define ZBX_REQUEST_ITEM_NAME		106
#define ZBX_REQUEST_ITEM_NAME_ORIG	107
#define ZBX_REQUEST_ITEM_KEY		108
#define ZBX_REQUEST_ITEM_KEY_ORIG	109
#define ZBX_REQUEST_ITEM_DESCRIPTION	110
#define ZBX_REQUEST_PROXY_NAME		111
#define ZBX_REQUEST_PROXY_DESCRIPTION	112

/* DBget_history_log_value() */
#define ZBX_REQUEST_ITEM_LOG_DATE	201
#define ZBX_REQUEST_ITEM_LOG_TIME	202
#define ZBX_REQUEST_ITEM_LOG_AGE	203
#define ZBX_REQUEST_ITEM_LOG_SOURCE	204
#define ZBX_REQUEST_ITEM_LOG_SEVERITY	205
#define ZBX_REQUEST_ITEM_LOG_NSEVERITY	206
#define ZBX_REQUEST_ITEM_LOG_EVENTID	207

/******************************************************************************
 *                                                                            *
 * Function: get_N_functionid                                                 *
 *                                                                            *
 * Parameters: expression   - [IN] null terminated trigger expression         *
 *                            '{11}=1 & {2346734}>5'                          *
 *             N_functionid - [IN] number of function in trigger expression   *
 *             functionid   - [OUT] ID of an N-th function in expression      *
 *             end          - [OUT] a pointer to text following the extracted *
 *                            function id (can be NULL)                       *
 *                                                                            *
 ******************************************************************************/
int	get_N_functionid(const char *expression, int N_functionid, zbx_uint64_t *functionid, const char **end)
{
	enum state_t {NORMAL, ID}	state = NORMAL;
	int				num = 0, ret = FAIL;
	const char			*c, *p_functionid = NULL;

	for (c = expression; '\0' != *c; c++)
	{
		if ('{' == *c)
		{
			/* skip user macros */
			if ('$' == c[1])
			{
				int	macro_r, context_l, context_r;

				if (SUCCEED == zbx_user_macro_parse(c, &macro_r, &context_l, &context_r))
					c += macro_r;
				else
					c++;

				continue;
			}

			state = ID;
			p_functionid = c + 1;
		}
		else if ('}' == *c && ID == state && NULL != p_functionid)
		{
			if (SUCCEED == is_uint64_n(p_functionid, c - p_functionid, functionid))
			{
				if (++num == N_functionid)
				{
					if (NULL != end)
						*end = c + 1;

					ret = SUCCEED;
					break;
				}
			}

			state = NORMAL;
		}
	}

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: get_functionids                                                  *
 *                                                                            *
 * Purpose: get identifiers of the functions used in expression               *
 *                                                                            *
 * Parameters: functionids - [OUT] the resulting vector of function ids       *
 *             expression  - [IN] null terminated trigger expression          *
 *                           '{11}=1 & {2346734}>5'                           *
 *                                                                            *
 ******************************************************************************/
void	get_functionids(zbx_vector_uint64_t *functionids, const char *expression)
{
	const char	*start = expression;
	zbx_uint64_t	functionid;

	if ('\0' == *expression)
		return;

	while (SUCCEED == get_N_functionid(start, 1, &functionid, &start))
		zbx_vector_uint64_append(functionids, functionid);

	zbx_vector_uint64_sort(functionids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
	zbx_vector_uint64_uniq(functionids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
}

/******************************************************************************
 *                                                                            *
 * Function: get_N_itemid                                                     *
 *                                                                            *
 * Parameters: expression   - [IN] null terminated trigger expression         *
 *                            '{11}=1 & {2346734}>5'                          *
 *             N_functionid - [IN] number of function in trigger expression   *
 *             itemid       - [OUT] ID of an item of N-th function in         *
 *                            expression                                      *
 *                                                                            *
 ******************************************************************************/
static int	get_N_itemid(const char *expression, int N_functionid, zbx_uint64_t *itemid)
{
	const char	*__function_name = "get_N_itemid";

	zbx_uint64_t	functionid;
	DC_FUNCTION	function;
	int		errcode, ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() expression:'%s' N_functionid:%d",
			__function_name, expression, N_functionid);

	if (SUCCEED == get_N_functionid(expression, N_functionid, &functionid, NULL))
	{
		DCconfig_get_functions_by_functionids(&function, &functionid, &errcode, 1);

		if (SUCCEED == errcode)
		{
			*itemid = function.itemid;
			ret = SUCCEED;
		}

		DCconfig_clean_functions(&function, &errcode, 1);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: extract_numbers                                                  *
 *                                                                            *
 * Purpose: Extract from string numbers with prefixes (A-Z)                   *
 *                                                                            *
 * Return value:                                                              *
 *                                                                            *
 * Author: Eugene Grigorjev                                                   *
 *                                                                            *
 * Comments: !!! Don't forget to sync the code with PHP !!!                   *
 *           Use zbx_free_numbers to free allocated memory                    *
 *                                                                            *
 ******************************************************************************/
static char 	**extract_numbers(const char *str, int *count)
{
	char		**result = NULL;
	const char	*s, *e;
	int		dot_found;
	size_t		len;

	assert(count);

	*count = 0;

	for (s = str; '\0' != *s; s++)	/* find start of number */
	{
		if (!isdigit(*s))
			continue;

		if (s != str && '{' == *(s - 1) && NULL != (e = strchr(s, '}')))
		{
			/* skip functions '{65432}' */
			s = e;
			continue;
		}

		dot_found = 0;

		for (e = s; '\0' != *e; e++)	/* find end of number */
		{
			if (isdigit(*e))
				continue;

			if ('.' == *e && 0 == dot_found)
			{
				dot_found = 1;
				continue;
			}

			if ('A' <= *e && *e <= 'Z')
				e++;

			break;
		}

		/* number found */

		len = e - s;
		(*count)++;
		result = zbx_realloc(result, sizeof(char *) * (*count));
		result[(*count) - 1] = zbx_malloc(NULL, len + 1);
		memcpy(result[(*count) - 1], s, len);
		result[(*count) - 1][len] = '\0';

		if ('\0' == *(s = e))
			break;
	}

	return result;
}

static void	zbx_free_numbers(char ***numbers, int count)
{
	int	i;

	if (NULL == numbers || NULL == *numbers)
		return;

	for (i = 0; i < count; i++)
		zbx_free((*numbers)[i]);

	zbx_free(*numbers);
}

/******************************************************************************
 *                                                                            *
 * Function: expand_trigger_description_constants                             *
 *                                                                            *
 * Purpose: substitute simple macros in data string with real values          *
 *                                                                            *
 * Parameters: data - trigger description                                     *
 *                                                                            *
 ******************************************************************************/
static void	expand_trigger_description_constants(char **data, const char *expression)
{
	char	**numbers = NULL, *new_str = NULL, replace[3] = "$0";
	int	numbers_cnt = 0, i = 0;

	numbers = extract_numbers(expression, &numbers_cnt);

	for (i = 0; i < 9; i++)
	{
		replace[1] = '0' + i + 1;
		new_str = string_replace(*data, replace, i < numbers_cnt ?  numbers[i] : "");
		zbx_free(*data);
		*data = new_str;
	}

	zbx_free_numbers(&numbers, numbers_cnt);
}

static void	DCexpand_trigger_expression(char **expression)
{
	const char	*__function_name = "DCexpand_trigger_expression";

	char		*tmp = NULL;
	size_t		tmp_alloc = 256, tmp_offset = 0, l, r;
	DC_FUNCTION	function;
	DC_ITEM		item;
	zbx_uint64_t	functionid;
	int		errcode[2];

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() expression:'%s'", __function_name, *expression);

	tmp = zbx_malloc(tmp, tmp_alloc);

	for (l = 0; '\0' != (*expression)[l]; l++)
	{
		if ('{' != (*expression)[l])
		{
			zbx_chrcpy_alloc(&tmp, &tmp_alloc, &tmp_offset, (*expression)[l]);
			continue;
		}

		/* skip user macros */
		if ('$' == (*expression)[l + 1])
		{
			int	macro_r, context_l, context_r;

			if (SUCCEED == zbx_user_macro_parse(*expression + l, &macro_r, &context_l, &context_r))
			{
				zbx_strncpy_alloc(&tmp, &tmp_alloc, &tmp_offset, *expression + l, macro_r + 1);
				l += macro_r;
				continue;
			}

			zbx_chrcpy_alloc(&tmp, &tmp_alloc, &tmp_offset, '{');
			zbx_chrcpy_alloc(&tmp, &tmp_alloc, &tmp_offset, '$');
			l++;
			continue;
		}

		for (r = l + 1; 0 != isdigit((*expression)[r]); r++)
			;

		if ('}' != (*expression)[r])
		{
			zbx_chrcpy_alloc(&tmp, &tmp_alloc, &tmp_offset, (*expression)[l]);
			continue;
		}

		(*expression)[r] = '\0';

		if (SUCCEED == is_uint64(&(*expression)[l + 1], &functionid))
		{
			DCconfig_get_functions_by_functionids(&function, &functionid, &errcode[0], 1);

			if (SUCCEED == errcode[0])
			{
				DCconfig_get_items_by_itemids(&item, &function.itemid, &errcode[1], 1);

				if (SUCCEED == errcode[1])
				{
					zbx_chrcpy_alloc(&tmp, &tmp_alloc, &tmp_offset, '{');
					zbx_strcpy_alloc(&tmp, &tmp_alloc, &tmp_offset, item.host.host);
					zbx_chrcpy_alloc(&tmp, &tmp_alloc, &tmp_offset, ':');
					zbx_strcpy_alloc(&tmp, &tmp_alloc, &tmp_offset, item.key_orig);
					zbx_chrcpy_alloc(&tmp, &tmp_alloc, &tmp_offset, '.');
					zbx_strcpy_alloc(&tmp, &tmp_alloc, &tmp_offset, function.function);
					zbx_chrcpy_alloc(&tmp, &tmp_alloc, &tmp_offset, '(');
					zbx_strcpy_alloc(&tmp, &tmp_alloc, &tmp_offset, function.parameter);
					zbx_strcpy_alloc(&tmp, &tmp_alloc, &tmp_offset, ")}");
				}

				DCconfig_clean_items(&item, &errcode[1], 1);
			}

			DCconfig_clean_functions(&function, &errcode[0], 1);

			if (SUCCEED != errcode[0] || SUCCEED != errcode[1])
				zbx_strcpy_alloc(&tmp, &tmp_alloc, &tmp_offset, "*ERROR*");

			l = r;
		}
		else
			zbx_chrcpy_alloc(&tmp, &tmp_alloc, &tmp_offset, (*expression)[l]);

		(*expression)[r] = '}';
	}

	zbx_free(*expression);
	*expression = tmp;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s() expression:'%s'", __function_name, *expression);
}

/******************************************************************************
 *                                                                            *
 * Function: item_description                                                 *
 *                                                                            *
 * Purpose: substitute key parameters and user macros in                      *
 *          the item description string with real values                      *
 *                                                                            *
 ******************************************************************************/
static void	item_description(char **data, const char *key, zbx_uint64_t hostid)
{
	AGENT_REQUEST	request;
	const char	*param;
	char		c, *p, *m, *n, *str_out = NULL, *replace_to = NULL;
	int		macro_r, context_l, context_r;

	init_request(&request);

	if (SUCCEED != parse_item_key(key, &request))
		goto out;

	p = *data;

	while (NULL != (m = strchr(p, '$')))
	{
		if (m > p && '{' == *(m - 1) && FAIL != zbx_user_macro_parse(m - 1, &macro_r, &context_l, &context_r))
		{
			/* user macros */

			n = m + macro_r;
			c = *n;
			*n = '\0';
			DCget_user_macro(&hostid, 1, m - 1, &replace_to);

			if (NULL != replace_to)
			{
				*(m - 1) = '\0';
				str_out = zbx_strdcat(str_out, p);
				*(m - 1) = '{';

				str_out = zbx_strdcat(str_out, replace_to);
				zbx_free(replace_to);
			}
			else
				str_out = zbx_strdcat(str_out, p);

			*n = c;
			p = n;
		}
		else if ('1' <= *(m + 1) && *(m + 1) <= '9')
		{
			/* macros $1, $2, ... */

			*m = '\0';
			str_out = zbx_strdcat(str_out, p);
			*m++ = '$';

			if (NULL != (param = get_rparam(&request, *m - '0' - 1)))
				str_out = zbx_strdcat(str_out, param);

			p = m + 1;
		}
		else
		{
			/* just a dollar sign */

			c = *++m;
			*m = '\0';
			str_out = zbx_strdcat(str_out, p);
			*m = c;
			p = m;
		}
	}

	if (NULL != str_out)
	{
		str_out = zbx_strdcat(str_out, p);
		zbx_free(*data);
		*data = str_out;
	}
out:
	free_request(&request);
}

/******************************************************************************
 *                                                                            *
 * Function: DBget_host_value                                                 *
 *                                                                            *
 * Purpose: request host name by hostid                                       *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 ******************************************************************************/
static int	DBget_host_value(zbx_uint64_t hostid, char **replace_to, const char *field_name)
{
	DB_RESULT	result;
	DB_ROW		row;
	int		ret = FAIL;

	result = DBselect(
			"select %s"
			" from hosts"
			" where hostid=" ZBX_FS_UI64,
			field_name, hostid);

	if (NULL != (row = DBfetch(result)))
	{
		*replace_to = zbx_strdup(*replace_to, row[0]);
		ret = SUCCEED;
	}
	DBfree_result(result);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: DBget_templateid_by_triggerid                                    *
 *                                                                            *
 * Purpose: get template trigger ID from which the trigger is inherited       *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 ******************************************************************************/
static int	DBget_templateid_by_triggerid(zbx_uint64_t triggerid, zbx_uint64_t *templateid)
{
	DB_RESULT	result;
	DB_ROW		row;
	int		ret = FAIL;

	result = DBselect(
			"select templateid"
			" from triggers"
			" where triggerid=" ZBX_FS_UI64,
			triggerid);

	if (NULL != (row = DBfetch(result)))
	{
		ZBX_DBROW2UINT64(*templateid, row[0]);
		ret = SUCCEED;
	}
	DBfree_result(result);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: DBget_trigger_template_name                                      *
 *                                                                            *
 * Purpose: get comma-space separated trigger template names in which         *
 *          the trigger is defined                                            *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 * Comments: based on the patch submitted by Hmami Mohamed                    *
 *                                                                            *
 ******************************************************************************/
static int	DBget_trigger_template_name(zbx_uint64_t triggerid, const zbx_uint64_t *userid, char **replace_to)
{
	const char	*__function_name = "DBget_trigger_template_name";

	DB_RESULT	result;
	DB_ROW		row;
	int		ret = FAIL;
	zbx_uint64_t	templateid;
	char		*sql = NULL;
	size_t		replace_to_alloc = 64, replace_to_offset = 0,
			sql_alloc = 256, sql_offset = 0;
	int		user_type = -1;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if (NULL != userid)
	{
		result = DBselect("select type from users where userid=" ZBX_FS_UI64, *userid);

		if (NULL != (row = DBfetch(result)) && FAIL == DBis_null(row[0]))
			user_type = atoi(row[0]);
		DBfree_result(result);

		if (-1 == user_type)
		{
			zabbix_log(LOG_LEVEL_DEBUG, "%s() cannot check permissions", __function_name);
			goto out;
		}
	}

	/* use parent trigger ID for lld generated triggers */
	result = DBselect(
			"select parent_triggerid"
			" from trigger_discovery"
			" where triggerid=" ZBX_FS_UI64,
			triggerid);

	if (NULL != (row = DBfetch(result)))
		ZBX_STR2UINT64(triggerid, row[0]);
	DBfree_result(result);

	if (SUCCEED != DBget_templateid_by_triggerid(triggerid, &templateid) || 0 == templateid)
	{
		zabbix_log(LOG_LEVEL_DEBUG, "%s() trigger not found or not templated", __function_name);
		goto out;
	}

	do
	{
		triggerid = templateid;
	}
	while (SUCCEED == (ret = DBget_templateid_by_triggerid(triggerid, &templateid)) && 0 != templateid);

	if (SUCCEED != ret)
	{
		zabbix_log(LOG_LEVEL_DEBUG, "%s() trigger not found", __function_name);
		goto out;
	}

	*replace_to = zbx_realloc(*replace_to, replace_to_alloc);
	**replace_to = '\0';

	sql = zbx_malloc(sql, sql_alloc);

	zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
			"select distinct h.name"
			" from hosts h,items i,functions f"
			" where h.hostid=i.hostid"
				" and i.itemid=f.itemid"
				" and f.triggerid=" ZBX_FS_UI64,
			triggerid);
	if (NULL != userid && USER_TYPE_SUPER_ADMIN != user_type)
	{
		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
				" and exists("
					"select null"
					" from hosts_groups hg,rights r,users_groups ug"
					" where h.hostid=hg.hostid"
						" and hg.groupid=r.id"
						" and r.groupid=ug.usrgrpid"
						" and ug.userid=" ZBX_FS_UI64
					" group by hg.hostid"
					" having min(r.permission)>=%d"
				")",
				*userid, PERM_READ);
	}
	zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, " order by h.name");

	result = DBselect("%s", sql);

	zbx_free(sql);

	while (NULL != (row = DBfetch(result)))
	{
		if (0 != replace_to_offset)
			zbx_strcpy_alloc(replace_to, &replace_to_alloc, &replace_to_offset, ", ");
		zbx_strcpy_alloc(replace_to, &replace_to_alloc, &replace_to_offset, row[0]);
	}
	DBfree_result(result);
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: DBget_trigger_hostgroup_name                                     *
 *                                                                            *
 * Purpose: get comma-space separated host group names in which the trigger   *
 *          is defined                                                        *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 ******************************************************************************/
static int	DBget_trigger_hostgroup_name(zbx_uint64_t triggerid, const zbx_uint64_t *userid, char **replace_to)
{
	const char	*__function_name = "DBget_trigger_hostgroup_name";

	DB_RESULT	result;
	DB_ROW		row;
	int		ret = FAIL;
	char		*sql = NULL;
	size_t		replace_to_alloc = 64, replace_to_offset = 0,
			sql_alloc = 256, sql_offset = 0;
	int		user_type = -1;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if (NULL != userid)
	{
		result = DBselect("select type from users where userid=" ZBX_FS_UI64, *userid);

		if (NULL != (row = DBfetch(result)) && FAIL == DBis_null(row[0]))
			user_type = atoi(row[0]);
		DBfree_result(result);

		if (-1 == user_type)
		{
			zabbix_log(LOG_LEVEL_DEBUG, "%s() cannot check permissions", __function_name);
			goto out;
		}
	}

	*replace_to = zbx_realloc(*replace_to, replace_to_alloc);
	**replace_to = '\0';

	sql = zbx_malloc(sql, sql_alloc);

	zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
			"select distinct g.name"
			" from groups g,hosts_groups hg,items i,functions f"
			" where g.groupid=hg.groupid"
				" and hg.hostid=i.hostid"
				" and i.itemid=f.itemid"
				" and f.triggerid=" ZBX_FS_UI64,
			triggerid);
	if (NULL != userid && USER_TYPE_SUPER_ADMIN != user_type)
	{
		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
				" and exists("
					"select null"
					" from rights r,users_groups ug"
					" where g.groupid=r.id"
						" and r.groupid=ug.usrgrpid"
						" and ug.userid=" ZBX_FS_UI64
					" group by r.id"
					" having min(r.permission)>=%d"
				")",
				*userid, PERM_READ);
	}
	zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, " order by g.name");

	result = DBselect("%s", sql);

	zbx_free(sql);

	while (NULL != (row = DBfetch(result)))
	{
		if (0 != replace_to_offset)
			zbx_strcpy_alloc(replace_to, &replace_to_alloc, &replace_to_offset, ", ");
		zbx_strcpy_alloc(replace_to, &replace_to_alloc, &replace_to_offset, row[0]);
		ret = SUCCEED;
	}
	DBfree_result(result);
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: get_interface_value                                              *
 *                                                                            *
 * Purpose: retrieve a particular value associated with the interface         *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 ******************************************************************************/
static int	get_interface_value(zbx_uint64_t hostid, zbx_uint64_t itemid, char **replace_to, int request)
{
	int		res;
	DC_INTERFACE	interface;

	if (SUCCEED != (res = DCconfig_get_interface(&interface, hostid, itemid)))
		return res;

	switch (request)
	{
		case ZBX_REQUEST_HOST_IP:
			*replace_to = zbx_strdup(*replace_to, interface.ip_orig);
			break;
		case ZBX_REQUEST_HOST_DNS:
			*replace_to = zbx_strdup(*replace_to, interface.dns_orig);
			break;
		case ZBX_REQUEST_HOST_CONN:
			*replace_to = zbx_strdup(*replace_to, interface.addr);
			break;
		case ZBX_REQUEST_HOST_PORT:
			*replace_to = zbx_strdup(*replace_to, interface.port_orig);
			break;
		default:
			THIS_SHOULD_NEVER_HAPPEN;
			res = FAIL;
	}

	return res;
}

/******************************************************************************
 *                                                                            *
 * Function: DBget_item_value                                                 *
 *                                                                            *
 * Purpose: retrieve a particular value associated with the item              *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 ******************************************************************************/
static int	DBget_item_value(zbx_uint64_t itemid, char **replace_to, int request)
{
	const char	*__function_name = "DBget_item_value";
	DB_RESULT	result;
	DB_ROW		row;
	DC_ITEM		dc_item;
	char		*key;
	zbx_uint64_t	proxy_hostid;
	int		ret = FAIL, errcode;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	switch (request)
	{
		case ZBX_REQUEST_HOST_IP:
		case ZBX_REQUEST_HOST_DNS:
		case ZBX_REQUEST_HOST_CONN:
		case ZBX_REQUEST_HOST_PORT:
			return get_interface_value(0, itemid, replace_to, request);
	}

	result = DBselect(
			"select h.hostid,h.proxy_hostid,h.host,h.name,h.description,"
				"i.itemid,i.name,i.key_,i.description"
			" from items i"
				" join hosts h on h.hostid=i.hostid"
			" where i.itemid=" ZBX_FS_UI64, itemid);

	if (NULL != (row = DBfetch(result)))
	{
		switch (request)
		{
			case ZBX_REQUEST_HOST_ID:
				*replace_to = zbx_strdup(*replace_to, row[0]);
				ret = SUCCEED;
				break;
			case ZBX_REQUEST_HOST_HOST:
				*replace_to = zbx_strdup(*replace_to, row[2]);
				ret = SUCCEED;
				break;
			case ZBX_REQUEST_HOST_NAME:
				*replace_to = zbx_strdup(*replace_to, row[3]);
				ret = SUCCEED;
				break;
			case ZBX_REQUEST_HOST_DESCRIPTION:
				*replace_to = zbx_strdup(*replace_to, row[4]);
				ret = SUCCEED;
				break;
			case ZBX_REQUEST_ITEM_ID:
				*replace_to = zbx_strdup(*replace_to, row[5]);
				ret = SUCCEED;
				break;
			case ZBX_REQUEST_ITEM_NAME:
			case ZBX_REQUEST_ITEM_KEY:
				DCconfig_get_items_by_itemids(&dc_item, &itemid, &errcode, 1);

				if (SUCCEED == errcode)
				{
					if (INTERFACE_TYPE_UNKNOWN == dc_item.interface.type)
						ret = DCconfig_get_interface(&dc_item.interface, dc_item.host.hostid, 0);
					else
						ret = SUCCEED;

					if (SUCCEED == ret)
					{
						key = zbx_strdup(NULL, dc_item.key_orig);
						substitute_key_macros(&key, NULL, &dc_item, NULL, MACRO_TYPE_ITEM_KEY,
								NULL, 0);

						if (ZBX_REQUEST_ITEM_NAME == request)
						{
							*replace_to = zbx_strdup(*replace_to, row[6]);
							item_description(replace_to, key, dc_item.host.hostid);
							zbx_free(key);
						}
						else	/* ZBX_REQUEST_ITEM_KEY */
						{
							zbx_free(*replace_to);
							*replace_to = key;
						}
					}

					DCconfig_clean_items(&dc_item, &errcode, 1);
				}
				break;
			case ZBX_REQUEST_ITEM_NAME_ORIG:
				*replace_to = zbx_strdup(*replace_to, row[6]);
				ret = SUCCEED;
				break;
			case ZBX_REQUEST_ITEM_KEY_ORIG:
				*replace_to = zbx_strdup(*replace_to, row[7]);
				ret = SUCCEED;
				break;
			case ZBX_REQUEST_ITEM_DESCRIPTION:
				*replace_to = zbx_strdup(*replace_to, row[8]);
				ret = SUCCEED;
				break;
			case ZBX_REQUEST_PROXY_NAME:
				ZBX_DBROW2UINT64(proxy_hostid, row[1]);

				if (0 == proxy_hostid)
				{
					*replace_to = zbx_strdup(*replace_to, "");
					ret = SUCCEED;
				}
				else
					ret = DBget_host_value(proxy_hostid, replace_to, "host");
				break;
			case ZBX_REQUEST_PROXY_DESCRIPTION:
				ZBX_DBROW2UINT64(proxy_hostid, row[1]);

				if (0 == proxy_hostid)
				{
					*replace_to = zbx_strdup(*replace_to, "");
					ret = SUCCEED;
				}
				else
					ret = DBget_host_value(proxy_hostid, replace_to, "description");
				break;
		}
	}
	DBfree_result(result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: DBget_trigger_value                                              *
 *                                                                            *
 * Purpose: retrieve a particular value associated with the trigger's         *
 *          N_functionid'th function                                          *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 ******************************************************************************/
static int	DBget_trigger_value(const char *expression, char **replace_to, int N_functionid, int request)
{
	const char	*__function_name = "DBget_trigger_value";

	zbx_uint64_t	itemid;
	int		ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if (SUCCEED == get_N_itemid(expression, N_functionid, &itemid))
		ret = DBget_item_value(itemid, replace_to, request);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: DBget_trigger_event_count                                        *
 *                                                                            *
 * Purpose: retrieve number of events (acknowledged or unacknowledged) for a  *
 *          trigger (in an OK or PROBLEM state) which generated an event      *
 *                                                                            *
 * Parameters: triggerid - trigger identifier from database                   *
 *             replace_to - pointer to result buffer                          *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 * Author: Alexander Vladishev, Aleksandrs Saveljevs                          *
 *                                                                            *
 * Comments:                                                                  *
 *                                                                            *
 ******************************************************************************/
static int	DBget_trigger_event_count(zbx_uint64_t triggerid, char **replace_to, int problem_only, int acknowledged)
{
	DB_RESULT	result;
	DB_ROW		row;
	char		value[4];
	int		ret = FAIL;

	if (problem_only)
		zbx_snprintf(value, sizeof(value), "%d", TRIGGER_VALUE_PROBLEM);
	else
		zbx_snprintf(value, sizeof(value), "%d,%d", TRIGGER_VALUE_PROBLEM, TRIGGER_VALUE_OK);

	result = DBselect(
			"select count(*)"
			" from events"
			" where source=%d"
				" and object=%d"
				" and objectid=" ZBX_FS_UI64
				" and value in (%s)"
				" and acknowledged=%d",
			EVENT_SOURCE_TRIGGERS,
			EVENT_OBJECT_TRIGGER,
			triggerid,
			value,
			acknowledged);

	if (NULL != (row = DBfetch(result)))
	{
		*replace_to = zbx_strdup(*replace_to, row[0]);
		ret = SUCCEED;
	}
	DBfree_result(result);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: DBget_dhost_value_by_event                                       *
 *                                                                            *
 * Purpose: retrieve discovered host value by event and field name            *
 *                                                                            *
 * Parameters:                                                                *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 * Comments:                                                                  *
 *                                                                            *
 ******************************************************************************/
static int	DBget_dhost_value_by_event(const DB_EVENT *event, char **replace_to, const char *fieldname)
{
	DB_RESULT	result;
	DB_ROW		row;
	int		ret = FAIL;
	char		sql[MAX_STRING_LEN];

	switch (event->object)
	{
		case EVENT_OBJECT_DHOST:
			zbx_snprintf(sql, sizeof(sql),
					"select %s"
					" from drules r,dhosts h,dservices s"
					" where r.druleid=h.druleid"
						" and h.dhostid=s.dhostid"
						" and h.dhostid=" ZBX_FS_UI64
					" order by s.dserviceid",
					fieldname,
					event->objectid);
			break;
		case EVENT_OBJECT_DSERVICE:
			zbx_snprintf(sql, sizeof(sql),
					"select %s"
					" from drules r,dhosts h,dservices s"
					" where r.druleid=h.druleid"
						" and h.dhostid=s.dhostid"
						" and s.dserviceid=" ZBX_FS_UI64,
					fieldname,
					event->objectid);
			break;
		default:
			return ret;
	}

	result = DBselectN(sql, 1);

	if (NULL != (row = DBfetch(result)) && SUCCEED != DBis_null(row[0]))
	{
		*replace_to = zbx_strdup(*replace_to, row[0]);
		ret = SUCCEED;
	}
	DBfree_result(result);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: DBget_dservice_value_by_event                                    *
 *                                                                            *
 * Purpose: retrieve discovered service value by event and field name         *
 *                                                                            *
 * Parameters:                                                                *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 * Comments:                                                                  *
 *                                                                            *
 ******************************************************************************/
static int	DBget_dservice_value_by_event(const DB_EVENT *event, char **replace_to, const char *fieldname)
{
	DB_RESULT	result;
	DB_ROW		row;
	int		ret = FAIL;

	switch (event->object)
	{
		case EVENT_OBJECT_DSERVICE:
			result = DBselect("select %s from dservices s where s.dserviceid=" ZBX_FS_UI64,
					fieldname, event->objectid);
			break;
		default:
			return ret;
	}

	if (NULL != (row = DBfetch(result)) && SUCCEED != DBis_null(row[0]))
	{
		*replace_to = zbx_strdup(*replace_to, row[0]);
		ret = SUCCEED;
	}

	DBfree_result(result);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: DBget_drule_value_by_event                                       *
 *                                                                            *
 * Purpose: retrieve discovery rule value by event and field name             *
 *                                                                            *
 * Parameters:                                                                *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 * Comments:                                                                  *
 *                                                                            *
 ******************************************************************************/
static int	DBget_drule_value_by_event(const DB_EVENT *event, char **replace_to, const char *fieldname)
{
	DB_RESULT	result;
	DB_ROW		row;
	int		ret = FAIL;

	if (EVENT_SOURCE_DISCOVERY != event->source)
		return FAIL;

	switch (event->object)
	{
		case EVENT_OBJECT_DHOST:
			result = DBselect("select r.%s from drules r,dhosts h"
					" where r.druleid=h.druleid and h.dhostid=" ZBX_FS_UI64,
					fieldname, event->objectid);
			break;
		case EVENT_OBJECT_DSERVICE:
			result = DBselect("select r.%s from drules r,dhosts h,dservices s"
					" where r.druleid=h.druleid and h.dhostid=s.dhostid and s.dserviceid=" ZBX_FS_UI64,
					fieldname, event->objectid);
			break;
		default:
			return ret;
	}

	if (NULL != (row = DBfetch(result)) && SUCCEED != DBis_null(row[0]))
	{
		*replace_to = zbx_strdup(*replace_to, row[0]);
		ret = SUCCEED;
	}
	DBfree_result(result);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: DBget_history_log_value                                          *
 *                                                                            *
 * Purpose: retrieve a particular attribute of a log value                    *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 ******************************************************************************/
static int	DBget_history_log_value(zbx_uint64_t itemid, char **replace_to, int request, int clock, int ns)
{
	const char		*__function_name = "DBget_history_log_value";

	DC_ITEM			item;
	int			ret = FAIL, errcode = FAIL;
	zbx_timespec_t		ts = {clock, ns};
	zbx_history_record_t	value;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	DCconfig_get_items_by_itemids(&item, &itemid, &errcode, 1);

	if (SUCCEED != errcode || ITEM_VALUE_TYPE_LOG != item.value_type)
		goto out;

	if (SUCCEED != zbx_vc_get_value(itemid, item.value_type, &ts, &value))
		goto out;

	switch (request)
	{
		case ZBX_REQUEST_ITEM_LOG_DATE:
			*replace_to = zbx_strdup(*replace_to, zbx_date2str((time_t)value.value.log->timestamp));
			goto success;
		case ZBX_REQUEST_ITEM_LOG_TIME:
			*replace_to = zbx_strdup(*replace_to, zbx_time2str((time_t)value.value.log->timestamp));
			goto success;
		case ZBX_REQUEST_ITEM_LOG_AGE:
			*replace_to = zbx_strdup(*replace_to, zbx_age2str(time(NULL) - value.value.log->timestamp));
			goto success;
	}

	/* the following attributes are set only for windows eventlog items */
	if (0 != strncmp(item.key_orig, "eventlog[", 9))
		goto clean;

	switch (request)
	{
		case ZBX_REQUEST_ITEM_LOG_SOURCE:
			*replace_to = zbx_strdup(*replace_to, (NULL == value.value.log->source ? "" :
					value.value.log->source));
			break;
		case ZBX_REQUEST_ITEM_LOG_SEVERITY:
			*replace_to = zbx_strdup(*replace_to,
					zbx_item_logtype_string((unsigned char)value.value.log->severity));
			break;
		case ZBX_REQUEST_ITEM_LOG_NSEVERITY:
			*replace_to = zbx_dsprintf(*replace_to, "%d", value.value.log->severity);
			break;
		case ZBX_REQUEST_ITEM_LOG_EVENTID:
			*replace_to = zbx_dsprintf(*replace_to, "%d", value.value.log->logeventid);
			break;
	}
success:
	ret = SUCCEED;
clean:
	zbx_history_record_clear(&value, ITEM_VALUE_TYPE_LOG);
out:
	DCconfig_clean_items(&item, &errcode, 1);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: get_history_log_value                                            *
 *                                                                            *
 * Purpose: retrieve a particular attribute of a log value                    *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 ******************************************************************************/
static int	get_history_log_value(const char *expression, char **replace_to, int N_functionid,
		int request, int clock, int ns)
{
	const char	*__function_name = "get_history_log_value";

	zbx_uint64_t	itemid;
	int		ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if (SUCCEED == get_N_itemid(expression, N_functionid, &itemid))
		ret = DBget_history_log_value(itemid, replace_to, request, clock, ns);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: DBitem_lastvalue                                                 *
 *                                                                            *
 * Purpose: retrieve item lastvalue by trigger expression                     *
 *          and number of function                                            *
 *                                                                            *
 * Parameters:                                                                *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 * Comments:                                                                  *
 *                                                                            *
 ******************************************************************************/
static int	DBitem_lastvalue(const char *expression, char **lastvalue, int N_functionid, int raw)
{
	const char	*__function_name = "DBitem_lastvalue";

	DB_RESULT	result;
	DB_ROW		row;
	zbx_uint64_t	itemid;
	int		ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if (FAIL == get_N_itemid(expression, N_functionid, &itemid))
		goto out;

	result = DBselect(
			"select value_type,valuemapid,units"
			" from items"
			" where itemid=" ZBX_FS_UI64,
			itemid);

	if (NULL != (row = DBfetch(result)))
	{
		unsigned char		value_type;
		zbx_uint64_t		valuemapid;
		zbx_history_record_t	vc_value;
		zbx_timespec_t		ts;

		ts.sec = time(NULL);
		ts.ns = 999999999;

		value_type = (unsigned char)atoi(row[0]);
		ZBX_DBROW2UINT64(valuemapid, row[1]);

		if (SUCCEED == zbx_vc_get_value(itemid, value_type, &ts, &vc_value))
		{
			char	tmp[MAX_BUFFER_LEN];

			zbx_vc_history_value2str(tmp, sizeof(tmp), &vc_value.value, value_type);
			zbx_history_record_clear(&vc_value, value_type);

			if (0 == raw)
				zbx_format_value(tmp, sizeof(tmp), valuemapid, row[2], value_type);

			*lastvalue = zbx_strdup(*lastvalue, tmp);

			ret = SUCCEED;
		}
	}
	DBfree_result(result);
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: DBitem_value                                                     *
 *                                                                            *
 * Purpose: retrieve item value by trigger expression and number of function  *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 ******************************************************************************/
static int	DBitem_value(const char *expression, char **value, int N_functionid, int clock, int ns, int raw)
{
	const char	*__function_name = "DBitem_value";

	DB_RESULT	result;
	DB_ROW		row;
	zbx_uint64_t	itemid;
	int		ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if (FAIL == get_N_itemid(expression, N_functionid, &itemid))
		goto out;

	result = DBselect(
			"select value_type,valuemapid,units"
			" from items"
			" where itemid=" ZBX_FS_UI64,
			itemid);

	if (NULL != (row = DBfetch(result)))
	{
		unsigned char		value_type;
		zbx_uint64_t		valuemapid;
		zbx_timespec_t		ts = {clock, ns};
		zbx_history_record_t	vc_value;

		value_type = (unsigned char)atoi(row[0]);
		ZBX_DBROW2UINT64(valuemapid, row[1]);

		if (SUCCEED == zbx_vc_get_value(itemid, value_type, &ts, &vc_value))
		{
			char	tmp[MAX_BUFFER_LEN];

			zbx_vc_history_value2str(tmp, sizeof(tmp), &vc_value.value, value_type);
			zbx_history_record_clear(&vc_value, value_type);

			if (0 == raw)
				zbx_format_value(tmp, sizeof(tmp), valuemapid, row[2], value_type);

			*value = zbx_strdup(*value, tmp);

			ret = SUCCEED;
		}
	}
	DBfree_result(result);
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: get_escalation_history                                           *
 *                                                                            *
 * Purpose: retrieve escalation history                                       *
 *                                                                            *
 ******************************************************************************/
static void	get_escalation_history(zbx_uint64_t actionid, const DB_EVENT *event, const DB_EVENT *r_event, char **replace_to)
{
	DB_RESULT	result;
	DB_ROW		row;
	char		*buf = NULL, *p;
	size_t		buf_alloc = ZBX_KIBIBYTE, buf_offset = 0;
	int		esc_step;
	unsigned char	type, status;
	time_t		now;
	zbx_uint64_t	userid;

	buf = zbx_malloc(buf, buf_alloc);

	zbx_snprintf_alloc(&buf, &buf_alloc, &buf_offset, "Problem started: %s %s Age: %s\n",
			zbx_date2str(event->clock), zbx_time2str(event->clock),
			zbx_age2str(time(NULL) - event->clock));

	result = DBselect("select a.clock,a.alerttype,a.status,mt.description,a.sendto"
				",a.error,a.esc_step,a.userid,a.message"
			" from alerts a"
			" left join media_type mt"
				" on mt.mediatypeid=a.mediatypeid"
			" where a.eventid=" ZBX_FS_UI64
				" and a.actionid=" ZBX_FS_UI64
			" order by a.clock",
			event->eventid, actionid);

	while (NULL != (row = DBfetch(result)))
	{
		now = atoi(row[0]);
		type = (unsigned char)atoi(row[1]);
		status = (unsigned char)atoi(row[2]);
		esc_step = atoi(row[6]);
		ZBX_DBROW2UINT64(userid, row[7]);

		if (0 != esc_step)
			zbx_snprintf_alloc(&buf, &buf_alloc, &buf_offset, "%d. ", esc_step);

		zbx_snprintf_alloc(&buf, &buf_alloc, &buf_offset, "%s %s %-7s %-11s",
				zbx_date2str(now), zbx_time2str(now),	/* date, time */
				zbx_alert_type_string(type),		/* alert type */
				zbx_alert_status_string(type, status));	/* alert status */

		if (ALERT_TYPE_COMMAND == type)
		{
			if (NULL != (p = strchr(row[8], ':')))
			{
				*p = '\0';
				zbx_snprintf_alloc(&buf, &buf_alloc, &buf_offset, " \"%s\"", row[8]);	/* host */
				*p = ':';
			}
		}
		else
		{
			zbx_snprintf_alloc(&buf, &buf_alloc, &buf_offset, " %s %s \"%s\"",
					SUCCEED == DBis_null(row[3]) ? "" : row[3],	/* media type description */
					row[4],						/* send to */
					zbx_user_string(userid));			/* alert user */
		}

		if (ALERT_STATUS_FAILED == status)
			zbx_snprintf_alloc(&buf, &buf_alloc, &buf_offset, " %s", row[5]);	/* alert error */

		zbx_chrcpy_alloc(&buf, &buf_alloc, &buf_offset, '\n');
	}
	DBfree_result(result);

	if (NULL != r_event)
	{
		zbx_snprintf_alloc(&buf, &buf_alloc, &buf_offset, "Problem ended: %s %s\n",
				zbx_date2str(r_event->clock), zbx_time2str(r_event->clock));
	}

	if (0 != buf_offset)
		buf[--buf_offset] = '\0';

	*replace_to = buf;
}

/******************************************************************************
 *                                                                            *
 * Function: acknowledge_expand_action_names                                  *
 *                                                                            *
 * Purpose: expand acknowledge action flags into user readable list           *
 *                                                                            *
 * Parameters: str        - [IN/OUT]                                          *
 *             str_alloc  - [IN/OUT]                                          *
 *             str_offset - [IN/OUT]                                          *
 *             action       [IN] the acknowledge action flags                 *
 *                               (see ZBX_ACKNOWLEDGE_ACTION_* defines)       *
 *                                                                            *
 ******************************************************************************/
static void	acknowledge_expand_action_names(char **str, size_t *str_alloc, size_t *str_offset, int action)
{
	if (0 != (action & ZBX_ACKNOWLEDGE_ACTION_CLOSE_PROBLEM))
		zbx_strcpy_alloc(str, str_alloc, str_offset, "Close problem");
}

/******************************************************************************
 *                                                                            *
 * Function: get_event_ack_history                                            *
 *                                                                            *
 * Purpose: retrieve event acknowledges history                               *
 *                                                                            *
 * Parameters:                                                                *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 * Comments:                                                                  *
 *                                                                            *
 ******************************************************************************/
static void	get_event_ack_history(const DB_EVENT *event, char **replace_to)
{
	DB_RESULT	result;
	DB_ROW		row;
	char		*buf = NULL;
	size_t		buf_alloc = ZBX_KIBIBYTE, buf_offset = 0;
	time_t		now;
	zbx_uint64_t	userid;
	int		action;

	if (0 == event->acknowledged)
	{
		*replace_to = zbx_strdup(*replace_to, "");
		return;
	}

	buf = zbx_malloc(buf, buf_alloc);
	*buf = '\0';

	result = DBselect("select clock,userid,message,action"
			" from acknowledges"
			" where eventid=" ZBX_FS_UI64 " order by clock",
			event->eventid);

	while (NULL != (row = DBfetch(result)))
	{
		now = atoi(row[0]);
		ZBX_STR2UINT64(userid, row[1]);
		action = atoi(row[3]);

		zbx_snprintf_alloc(&buf, &buf_alloc, &buf_offset,
				"%s %s \"%s\"\n",
				zbx_date2str(now),
				zbx_time2str(now),
				zbx_user_string(userid));

		if (ZBX_ACKNOWLEDGE_ACTION_NONE != action)
		{
			zbx_strcpy_alloc(&buf, &buf_alloc, &buf_offset, "Action: ");
			acknowledge_expand_action_names(&buf, &buf_alloc, &buf_offset, action);
			zbx_chrcpy_alloc(&buf, &buf_alloc, &buf_offset, '\n');
		}

		zbx_snprintf_alloc(&buf, &buf_alloc, &buf_offset, "%s\n\n", row[2]);
	}

	DBfree_result(result);

	if (0 != buf_offset)
	{
		buf_offset -= 2;
		buf[buf_offset] = '\0';
	}

	*replace_to = buf;
}

/******************************************************************************
 *                                                                            *
 * Function: get_autoreg_value_by_event                                       *
 *                                                                            *
 * Purpose: request value from autoreg_host table by event                    *
 *                                                                            *
 * Parameters:                                                                *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 * Comments:                                                                  *
 *                                                                            *
 ******************************************************************************/
static int	get_autoreg_value_by_event(const DB_EVENT *event, char **replace_to, const char *fieldname)
{
	DB_RESULT	result;
	DB_ROW		row;
	int		ret = FAIL;

	result = DBselect(
			"select %s"
			" from autoreg_host"
			" where autoreg_hostid=" ZBX_FS_UI64, fieldname, event->objectid);

	if (NULL != (row = DBfetch(result)))
	{
		if (SUCCEED == DBis_null(row[0]))
		{
			zbx_free(*replace_to);
		}
		else
			*replace_to = zbx_strdup(*replace_to, row[0]);

		ret = SUCCEED;
	}
	DBfree_result(result);

	return ret;
}

#define MVAR_ACTION			"{ACTION."			/* a prefix for all action macros */
#define MVAR_ACTION_ID			MVAR_ACTION "ID}"
#define MVAR_ACTION_NAME		MVAR_ACTION "NAME}"
#define MVAR_DATE			"{DATE}"
#define MVAR_EVENT			"{EVENT."			/* a prefix for all event macros */
#define MVAR_EVENT_ACK_HISTORY		MVAR_EVENT "ACK.HISTORY}"
#define MVAR_EVENT_ACK_STATUS		MVAR_EVENT "ACK.STATUS}"
#define MVAR_EVENT_AGE			MVAR_EVENT "AGE}"
#define MVAR_EVENT_DATE			MVAR_EVENT "DATE}"
#define MVAR_EVENT_ID			MVAR_EVENT "ID}"
#define MVAR_EVENT_STATUS		MVAR_EVENT "STATUS}"
#define MVAR_EVENT_TIME			MVAR_EVENT "TIME}"
#define MVAR_EVENT_VALUE		MVAR_EVENT "VALUE}"
#define MVAR_EVENT_TAGS			MVAR_EVENT "TAGS}"
#define MVAR_EVENT_RECOVERY		MVAR_EVENT "RECOVERY."		/* a prefix for all recovery event macros */
#define MVAR_EVENT_RECOVERY_DATE	MVAR_EVENT_RECOVERY "DATE}"
#define MVAR_EVENT_RECOVERY_ID		MVAR_EVENT_RECOVERY "ID}"
#define MVAR_EVENT_RECOVERY_STATUS	MVAR_EVENT_RECOVERY "STATUS}"
#define MVAR_EVENT_RECOVERY_TIME	MVAR_EVENT_RECOVERY "TIME}"
#define MVAR_EVENT_RECOVERY_VALUE	MVAR_EVENT_RECOVERY "VALUE}"
#define MVAR_EVENT_RECOVERY_TAGS	MVAR_EVENT_RECOVERY "TAGS}"

#define MVAR_ESC_HISTORY		"{ESC.HISTORY}"
#define MVAR_PROXY_NAME			"{PROXY.NAME}"
#define MVAR_PROXY_DESCRIPTION		"{PROXY.DESCRIPTION}"
#define MVAR_HOST_DNS			"{HOST.DNS}"
#define MVAR_HOST_CONN			"{HOST.CONN}"
#define MVAR_HOST_HOST			"{HOST.HOST}"
#define MVAR_HOST_ID			"{HOST.ID}"
#define MVAR_HOST_IP			"{HOST.IP}"
#define MVAR_IPADDRESS			"{IPADDRESS}"			/* deprecated */
#define MVAR_HOST_METADATA		"{HOST.METADATA}"
#define MVAR_HOST_NAME			"{HOST.NAME}"
#define MVAR_HOSTNAME			"{HOSTNAME}"			/* deprecated */
#define MVAR_HOST_DESCRIPTION		"{HOST.DESCRIPTION}"
#define MVAR_HOST_PORT			"{HOST.PORT}"
#define MVAR_TIME			"{TIME}"
#define MVAR_ITEM_LASTVALUE		"{ITEM.LASTVALUE}"
#define MVAR_ITEM_VALUE			"{ITEM.VALUE}"
#define MVAR_ITEM_ID			"{ITEM.ID}"
#define MVAR_ITEM_NAME			"{ITEM.NAME}"
#define MVAR_ITEM_NAME_ORIG		"{ITEM.NAME.ORIG}"
#define MVAR_ITEM_KEY			"{ITEM.KEY}"
#define MVAR_ITEM_KEY_ORIG		"{ITEM.KEY.ORIG}"
#define MVAR_ITEM_STATE			"{ITEM.STATE}"
#define MVAR_TRIGGER_KEY		"{TRIGGER.KEY}"			/* deprecated */
#define MVAR_ITEM_DESCRIPTION		"{ITEM.DESCRIPTION}"
#define MVAR_ITEM_LOG_DATE		"{ITEM.LOG.DATE}"
#define MVAR_ITEM_LOG_TIME		"{ITEM.LOG.TIME}"
#define MVAR_ITEM_LOG_AGE		"{ITEM.LOG.AGE}"
#define MVAR_ITEM_LOG_SOURCE		"{ITEM.LOG.SOURCE}"
#define MVAR_ITEM_LOG_SEVERITY		"{ITEM.LOG.SEVERITY}"
#define MVAR_ITEM_LOG_NSEVERITY		"{ITEM.LOG.NSEVERITY}"
#define MVAR_ITEM_LOG_EVENTID		"{ITEM.LOG.EVENTID}"

#define MVAR_TRIGGER_DESCRIPTION		"{TRIGGER.DESCRIPTION}"
#define MVAR_TRIGGER_COMMENT			"{TRIGGER.COMMENT}"		/* deprecated */
#define MVAR_TRIGGER_ID				"{TRIGGER.ID}"
#define MVAR_TRIGGER_NAME			"{TRIGGER.NAME}"
#define MVAR_TRIGGER_NAME_ORIG			"{TRIGGER.NAME.ORIG}"
#define MVAR_TRIGGER_EXPRESSION			"{TRIGGER.EXPRESSION}"
#define MVAR_TRIGGER_EXPRESSION_RECOVERY	"{TRIGGER.EXPRESSION.RECOVERY}"
#define MVAR_TRIGGER_SEVERITY			"{TRIGGER.SEVERITY}"
#define MVAR_TRIGGER_NSEVERITY			"{TRIGGER.NSEVERITY}"
#define MVAR_TRIGGER_STATUS			"{TRIGGER.STATUS}"
#define MVAR_TRIGGER_STATE			"{TRIGGER.STATE}"
#define MVAR_TRIGGER_TEMPLATE_NAME		"{TRIGGER.TEMPLATE.NAME}"
#define MVAR_TRIGGER_HOSTGROUP_NAME		"{TRIGGER.HOSTGROUP.NAME}"
#define MVAR_STATUS				"{STATUS}"			/* deprecated */
#define MVAR_TRIGGER_VALUE			"{TRIGGER.VALUE}"
#define MVAR_TRIGGER_URL			"{TRIGGER.URL}"

#define MVAR_TRIGGER_EVENTS_ACK			"{TRIGGER.EVENTS.ACK}"
#define MVAR_TRIGGER_EVENTS_UNACK		"{TRIGGER.EVENTS.UNACK}"
#define MVAR_TRIGGER_EVENTS_PROBLEM_ACK		"{TRIGGER.EVENTS.PROBLEM.ACK}"
#define MVAR_TRIGGER_EVENTS_PROBLEM_UNACK	"{TRIGGER.EVENTS.PROBLEM.UNACK}"

#define MVAR_LLDRULE_DESCRIPTION		"{LLDRULE.DESCRIPTION}"
#define MVAR_LLDRULE_ID				"{LLDRULE.ID}"
#define MVAR_LLDRULE_KEY			"{LLDRULE.KEY}"
#define MVAR_LLDRULE_KEY_ORIG			"{LLDRULE.KEY.ORIG}"
#define MVAR_LLDRULE_NAME			"{LLDRULE.NAME}"
#define MVAR_LLDRULE_NAME_ORIG			"{LLDRULE.NAME.ORIG}"
#define MVAR_LLDRULE_STATE			"{LLDRULE.STATE}"

#define MVAR_INVENTORY				"{INVENTORY."			/* a prefix for all inventory macros */
#define MVAR_INVENTORY_TYPE			MVAR_INVENTORY "TYPE}"
#define MVAR_INVENTORY_TYPE_FULL		MVAR_INVENTORY "TYPE.FULL}"
#define MVAR_INVENTORY_NAME			MVAR_INVENTORY "NAME}"
#define MVAR_INVENTORY_ALIAS			MVAR_INVENTORY "ALIAS}"
#define MVAR_INVENTORY_OS			MVAR_INVENTORY "OS}"
#define MVAR_INVENTORY_OS_FULL			MVAR_INVENTORY "OS.FULL}"
#define MVAR_INVENTORY_OS_SHORT			MVAR_INVENTORY "OS.SHORT}"
#define MVAR_INVENTORY_SERIALNO_A		MVAR_INVENTORY "SERIALNO.A}"
#define MVAR_INVENTORY_SERIALNO_B		MVAR_INVENTORY "SERIALNO.B}"
#define MVAR_INVENTORY_TAG			MVAR_INVENTORY "TAG}"
#define MVAR_INVENTORY_ASSET_TAG		MVAR_INVENTORY "ASSET.TAG}"
#define MVAR_INVENTORY_MACADDRESS_A		MVAR_INVENTORY "MACADDRESS.A}"
#define MVAR_INVENTORY_MACADDRESS_B		MVAR_INVENTORY "MACADDRESS.B}"
#define MVAR_INVENTORY_HARDWARE			MVAR_INVENTORY "HARDWARE}"
#define MVAR_INVENTORY_HARDWARE_FULL		MVAR_INVENTORY "HARDWARE.FULL}"
#define MVAR_INVENTORY_SOFTWARE			MVAR_INVENTORY "SOFTWARE}"
#define MVAR_INVENTORY_SOFTWARE_FULL		MVAR_INVENTORY "SOFTWARE.FULL}"
#define MVAR_INVENTORY_SOFTWARE_APP_A		MVAR_INVENTORY "SOFTWARE.APP.A}"
#define MVAR_INVENTORY_SOFTWARE_APP_B		MVAR_INVENTORY "SOFTWARE.APP.B}"
#define MVAR_INVENTORY_SOFTWARE_APP_C		MVAR_INVENTORY "SOFTWARE.APP.C}"
#define MVAR_INVENTORY_SOFTWARE_APP_D		MVAR_INVENTORY "SOFTWARE.APP.D}"
#define MVAR_INVENTORY_SOFTWARE_APP_E		MVAR_INVENTORY "SOFTWARE.APP.E}"
#define MVAR_INVENTORY_CONTACT			MVAR_INVENTORY "CONTACT}"
#define MVAR_INVENTORY_LOCATION			MVAR_INVENTORY "LOCATION}"
#define MVAR_INVENTORY_LOCATION_LAT		MVAR_INVENTORY "LOCATION.LAT}"
#define MVAR_INVENTORY_LOCATION_LON		MVAR_INVENTORY "LOCATION.LON}"
#define MVAR_INVENTORY_NOTES			MVAR_INVENTORY "NOTES}"
#define MVAR_INVENTORY_CHASSIS			MVAR_INVENTORY "CHASSIS}"
#define MVAR_INVENTORY_MODEL			MVAR_INVENTORY "MODEL}"
#define MVAR_INVENTORY_HW_ARCH			MVAR_INVENTORY "HW.ARCH}"
#define MVAR_INVENTORY_VENDOR			MVAR_INVENTORY "VENDOR}"
#define MVAR_INVENTORY_CONTRACT_NUMBER		MVAR_INVENTORY "CONTRACT.NUMBER}"
#define MVAR_INVENTORY_INSTALLER_NAME		MVAR_INVENTORY "INSTALLER.NAME}"
#define MVAR_INVENTORY_DEPLOYMENT_STATUS	MVAR_INVENTORY "DEPLOYMENT.STATUS}"
#define MVAR_INVENTORY_URL_A			MVAR_INVENTORY "URL.A}"
#define MVAR_INVENTORY_URL_B			MVAR_INVENTORY "URL.B}"
#define MVAR_INVENTORY_URL_C			MVAR_INVENTORY "URL.C}"
#define MVAR_INVENTORY_HOST_NETWORKS		MVAR_INVENTORY "HOST.NETWORKS}"
#define MVAR_INVENTORY_HOST_NETMASK		MVAR_INVENTORY "HOST.NETMASK}"
#define MVAR_INVENTORY_HOST_ROUTER		MVAR_INVENTORY "HOST.ROUTER}"
#define MVAR_INVENTORY_OOB_IP			MVAR_INVENTORY "OOB.IP}"
#define MVAR_INVENTORY_OOB_NETMASK		MVAR_INVENTORY "OOB.NETMASK}"
#define MVAR_INVENTORY_OOB_ROUTER		MVAR_INVENTORY "OOB.ROUTER}"
#define MVAR_INVENTORY_HW_DATE_PURCHASE		MVAR_INVENTORY "HW.DATE.PURCHASE}"
#define MVAR_INVENTORY_HW_DATE_INSTALL		MVAR_INVENTORY "HW.DATE.INSTALL}"
#define MVAR_INVENTORY_HW_DATE_EXPIRY		MVAR_INVENTORY "HW.DATE.EXPIRY}"
#define MVAR_INVENTORY_HW_DATE_DECOMM		MVAR_INVENTORY "HW.DATE.DECOMM}"
#define MVAR_INVENTORY_SITE_ADDRESS_A		MVAR_INVENTORY "SITE.ADDRESS.A}"
#define MVAR_INVENTORY_SITE_ADDRESS_B		MVAR_INVENTORY "SITE.ADDRESS.B}"
#define MVAR_INVENTORY_SITE_ADDRESS_C		MVAR_INVENTORY "SITE.ADDRESS.C}"
#define MVAR_INVENTORY_SITE_CITY		MVAR_INVENTORY "SITE.CITY}"
#define MVAR_INVENTORY_SITE_STATE		MVAR_INVENTORY "SITE.STATE}"
#define MVAR_INVENTORY_SITE_COUNTRY		MVAR_INVENTORY "SITE.COUNTRY}"
#define MVAR_INVENTORY_SITE_ZIP			MVAR_INVENTORY "SITE.ZIP}"
#define MVAR_INVENTORY_SITE_RACK		MVAR_INVENTORY "SITE.RACK}"
#define MVAR_INVENTORY_SITE_NOTES		MVAR_INVENTORY "SITE.NOTES}"
#define MVAR_INVENTORY_POC_PRIMARY_NAME		MVAR_INVENTORY "POC.PRIMARY.NAME}"
#define MVAR_INVENTORY_POC_PRIMARY_EMAIL	MVAR_INVENTORY "POC.PRIMARY.EMAIL}"
#define MVAR_INVENTORY_POC_PRIMARY_PHONE_A	MVAR_INVENTORY "POC.PRIMARY.PHONE.A}"
#define MVAR_INVENTORY_POC_PRIMARY_PHONE_B	MVAR_INVENTORY "POC.PRIMARY.PHONE.B}"
#define MVAR_INVENTORY_POC_PRIMARY_CELL		MVAR_INVENTORY "POC.PRIMARY.CELL}"
#define MVAR_INVENTORY_POC_PRIMARY_SCREEN	MVAR_INVENTORY "POC.PRIMARY.SCREEN}"
#define MVAR_INVENTORY_POC_PRIMARY_NOTES	MVAR_INVENTORY "POC.PRIMARY.NOTES}"
#define MVAR_INVENTORY_POC_SECONDARY_NAME	MVAR_INVENTORY "POC.SECONDARY.NAME}"
#define MVAR_INVENTORY_POC_SECONDARY_EMAIL	MVAR_INVENTORY "POC.SECONDARY.EMAIL}"
#define MVAR_INVENTORY_POC_SECONDARY_PHONE_A	MVAR_INVENTORY "POC.SECONDARY.PHONE.A}"
#define MVAR_INVENTORY_POC_SECONDARY_PHONE_B	MVAR_INVENTORY "POC.SECONDARY.PHONE.B}"
#define MVAR_INVENTORY_POC_SECONDARY_CELL	MVAR_INVENTORY "POC.SECONDARY.CELL}"
#define MVAR_INVENTORY_POC_SECONDARY_SCREEN	MVAR_INVENTORY "POC.SECONDARY.SCREEN}"
#define MVAR_INVENTORY_POC_SECONDARY_NOTES	MVAR_INVENTORY "POC.SECONDARY.NOTES}"

/* PROFILE.* is deprecated, use INVENTORY.* instead */
#define MVAR_PROFILE			"{PROFILE."			/* prefix for profile macros */
#define MVAR_PROFILE_DEVICETYPE		MVAR_PROFILE "DEVICETYPE}"
#define MVAR_PROFILE_NAME		MVAR_PROFILE "NAME}"
#define MVAR_PROFILE_OS			MVAR_PROFILE "OS}"
#define MVAR_PROFILE_SERIALNO		MVAR_PROFILE "SERIALNO}"
#define MVAR_PROFILE_TAG		MVAR_PROFILE "TAG}"
#define MVAR_PROFILE_MACADDRESS		MVAR_PROFILE "MACADDRESS}"
#define MVAR_PROFILE_HARDWARE		MVAR_PROFILE "HARDWARE}"
#define MVAR_PROFILE_SOFTWARE		MVAR_PROFILE "SOFTWARE}"
#define MVAR_PROFILE_CONTACT		MVAR_PROFILE "CONTACT}"
#define MVAR_PROFILE_LOCATION		MVAR_PROFILE "LOCATION}"
#define MVAR_PROFILE_NOTES		MVAR_PROFILE "NOTES}"

#define MVAR_DISCOVERY_RULE_NAME	"{DISCOVERY.RULE.NAME}"
#define MVAR_DISCOVERY_SERVICE_NAME	"{DISCOVERY.SERVICE.NAME}"
#define MVAR_DISCOVERY_SERVICE_PORT	"{DISCOVERY.SERVICE.PORT}"
#define MVAR_DISCOVERY_SERVICE_STATUS	"{DISCOVERY.SERVICE.STATUS}"
#define MVAR_DISCOVERY_SERVICE_UPTIME	"{DISCOVERY.SERVICE.UPTIME}"
#define MVAR_DISCOVERY_DEVICE_IPADDRESS	"{DISCOVERY.DEVICE.IPADDRESS}"
#define MVAR_DISCOVERY_DEVICE_DNS	"{DISCOVERY.DEVICE.DNS}"
#define MVAR_DISCOVERY_DEVICE_STATUS	"{DISCOVERY.DEVICE.STATUS}"
#define MVAR_DISCOVERY_DEVICE_UPTIME	"{DISCOVERY.DEVICE.UPTIME}"

#define MVAR_ALERT_SENDTO		"{ALERT.SENDTO}"
#define MVAR_ALERT_SUBJECT		"{ALERT.SUBJECT}"
#define MVAR_ALERT_MESSAGE		"{ALERT.MESSAGE}"

#define STR_UNKNOWN_VARIABLE		"*UNKNOWN*"

static const char	*ex_macros[] =
{
	MVAR_INVENTORY_TYPE, MVAR_INVENTORY_TYPE_FULL,
	MVAR_INVENTORY_NAME, MVAR_INVENTORY_ALIAS, MVAR_INVENTORY_OS, MVAR_INVENTORY_OS_FULL, MVAR_INVENTORY_OS_SHORT,
	MVAR_INVENTORY_SERIALNO_A, MVAR_INVENTORY_SERIALNO_B, MVAR_INVENTORY_TAG,
	MVAR_INVENTORY_ASSET_TAG, MVAR_INVENTORY_MACADDRESS_A, MVAR_INVENTORY_MACADDRESS_B,
	MVAR_INVENTORY_HARDWARE, MVAR_INVENTORY_HARDWARE_FULL, MVAR_INVENTORY_SOFTWARE, MVAR_INVENTORY_SOFTWARE_FULL,
	MVAR_INVENTORY_SOFTWARE_APP_A, MVAR_INVENTORY_SOFTWARE_APP_B, MVAR_INVENTORY_SOFTWARE_APP_C,
	MVAR_INVENTORY_SOFTWARE_APP_D, MVAR_INVENTORY_SOFTWARE_APP_E, MVAR_INVENTORY_CONTACT, MVAR_INVENTORY_LOCATION,
	MVAR_INVENTORY_LOCATION_LAT, MVAR_INVENTORY_LOCATION_LON, MVAR_INVENTORY_NOTES, MVAR_INVENTORY_CHASSIS,
	MVAR_INVENTORY_MODEL, MVAR_INVENTORY_HW_ARCH, MVAR_INVENTORY_VENDOR, MVAR_INVENTORY_CONTRACT_NUMBER,
	MVAR_INVENTORY_INSTALLER_NAME, MVAR_INVENTORY_DEPLOYMENT_STATUS, MVAR_INVENTORY_URL_A, MVAR_INVENTORY_URL_B,
	MVAR_INVENTORY_URL_C, MVAR_INVENTORY_HOST_NETWORKS, MVAR_INVENTORY_HOST_NETMASK, MVAR_INVENTORY_HOST_ROUTER,
	MVAR_INVENTORY_OOB_IP, MVAR_INVENTORY_OOB_NETMASK, MVAR_INVENTORY_OOB_ROUTER, MVAR_INVENTORY_HW_DATE_PURCHASE,
	MVAR_INVENTORY_HW_DATE_INSTALL, MVAR_INVENTORY_HW_DATE_EXPIRY, MVAR_INVENTORY_HW_DATE_DECOMM,
	MVAR_INVENTORY_SITE_ADDRESS_A, MVAR_INVENTORY_SITE_ADDRESS_B, MVAR_INVENTORY_SITE_ADDRESS_C,
	MVAR_INVENTORY_SITE_CITY, MVAR_INVENTORY_SITE_STATE, MVAR_INVENTORY_SITE_COUNTRY, MVAR_INVENTORY_SITE_ZIP,
	MVAR_INVENTORY_SITE_RACK, MVAR_INVENTORY_SITE_NOTES, MVAR_INVENTORY_POC_PRIMARY_NAME,
	MVAR_INVENTORY_POC_PRIMARY_EMAIL, MVAR_INVENTORY_POC_PRIMARY_PHONE_A, MVAR_INVENTORY_POC_PRIMARY_PHONE_B,
	MVAR_INVENTORY_POC_PRIMARY_CELL, MVAR_INVENTORY_POC_PRIMARY_SCREEN, MVAR_INVENTORY_POC_PRIMARY_NOTES,
	MVAR_INVENTORY_POC_SECONDARY_NAME, MVAR_INVENTORY_POC_SECONDARY_EMAIL, MVAR_INVENTORY_POC_SECONDARY_PHONE_A,
	MVAR_INVENTORY_POC_SECONDARY_PHONE_B, MVAR_INVENTORY_POC_SECONDARY_CELL, MVAR_INVENTORY_POC_SECONDARY_SCREEN,
	MVAR_INVENTORY_POC_SECONDARY_NOTES,
	/* PROFILE.* is deprecated, use INVENTORY.* instead */
	MVAR_PROFILE_DEVICETYPE, MVAR_PROFILE_NAME, MVAR_PROFILE_OS, MVAR_PROFILE_SERIALNO,
	MVAR_PROFILE_TAG, MVAR_PROFILE_MACADDRESS, MVAR_PROFILE_HARDWARE, MVAR_PROFILE_SOFTWARE,
	MVAR_PROFILE_CONTACT, MVAR_PROFILE_LOCATION, MVAR_PROFILE_NOTES,
	MVAR_HOST_HOST, MVAR_HOSTNAME, MVAR_HOST_NAME, MVAR_HOST_DESCRIPTION, MVAR_PROXY_NAME, MVAR_PROXY_DESCRIPTION,
	MVAR_HOST_CONN, MVAR_HOST_DNS, MVAR_HOST_IP, MVAR_HOST_PORT, MVAR_IPADDRESS, MVAR_HOST_ID,
	MVAR_ITEM_ID, MVAR_ITEM_NAME, MVAR_ITEM_NAME_ORIG, MVAR_ITEM_DESCRIPTION,
	MVAR_ITEM_KEY, MVAR_ITEM_KEY_ORIG, MVAR_TRIGGER_KEY,
	MVAR_ITEM_LASTVALUE,
	MVAR_ITEM_STATE,
	MVAR_ITEM_VALUE,
	MVAR_ITEM_LOG_DATE, MVAR_ITEM_LOG_TIME, MVAR_ITEM_LOG_AGE, MVAR_ITEM_LOG_SOURCE,
	MVAR_ITEM_LOG_SEVERITY, MVAR_ITEM_LOG_NSEVERITY, MVAR_ITEM_LOG_EVENTID,
	NULL
};

typedef struct
{
	const char	*macro;
	const char	*field_name;
} inventory_field_t;

static inventory_field_t	inventory_fields[] =
{
	{MVAR_INVENTORY_TYPE, "type"},
	{MVAR_PROFILE_DEVICETYPE, "type"},	/* deprecated */
	{MVAR_INVENTORY_TYPE_FULL, "type_full"},
	{MVAR_INVENTORY_NAME, "name"},
	{MVAR_PROFILE_NAME, "name"},	/* deprecated */
	{MVAR_INVENTORY_ALIAS, "alias"},
	{MVAR_INVENTORY_OS, "os"},
	{MVAR_PROFILE_OS, "os"},	/* deprecated */
	{MVAR_INVENTORY_OS_FULL, "os_full"},
	{MVAR_INVENTORY_OS_SHORT, "os_short"},
	{MVAR_INVENTORY_SERIALNO_A, "serialno_a"},
	{MVAR_PROFILE_SERIALNO, "serialno_a"},	/* deprecated */
	{MVAR_INVENTORY_SERIALNO_B, "serialno_b"},
	{MVAR_INVENTORY_TAG, "tag"},
	{MVAR_PROFILE_TAG, "tag"},	/* deprecated */
	{MVAR_INVENTORY_ASSET_TAG, "asset_tag"},
	{MVAR_INVENTORY_MACADDRESS_A, "macaddress_a"},
	{MVAR_PROFILE_MACADDRESS, "macaddress_a"},	/* deprecated */
	{MVAR_INVENTORY_MACADDRESS_B, "macaddress_b"},
	{MVAR_INVENTORY_HARDWARE, "hardware"},
	{MVAR_PROFILE_HARDWARE, "hardware"},	/* deprecated */
	{MVAR_INVENTORY_HARDWARE_FULL, "hardware_full"},
	{MVAR_INVENTORY_SOFTWARE, "software"},
	{MVAR_PROFILE_SOFTWARE, "software"},	/* deprecated */
	{MVAR_INVENTORY_SOFTWARE_FULL, "software_full"},
	{MVAR_INVENTORY_SOFTWARE_APP_A, "software_app_a"},
	{MVAR_INVENTORY_SOFTWARE_APP_B, "software_app_b"},
	{MVAR_INVENTORY_SOFTWARE_APP_C, "software_app_c"},
	{MVAR_INVENTORY_SOFTWARE_APP_D, "software_app_d"},
	{MVAR_INVENTORY_SOFTWARE_APP_E, "software_app_e"},
	{MVAR_INVENTORY_CONTACT, "contact"},
	{MVAR_PROFILE_CONTACT, "contact"},	/* deprecated */
	{MVAR_INVENTORY_LOCATION, "location"},
	{MVAR_PROFILE_LOCATION, "location"},	/* deprecated */
	{MVAR_INVENTORY_LOCATION_LAT, "location_lat"},
	{MVAR_INVENTORY_LOCATION_LON, "location_lon"},
	{MVAR_INVENTORY_NOTES, "notes"},
	{MVAR_PROFILE_NOTES, "notes"},	/* deprecated */
	{MVAR_INVENTORY_CHASSIS, "chassis"},
	{MVAR_INVENTORY_MODEL, "model"},
	{MVAR_INVENTORY_HW_ARCH, "hw_arch"},
	{MVAR_INVENTORY_VENDOR, "vendor"},
	{MVAR_INVENTORY_CONTRACT_NUMBER, "contract_number"},
	{MVAR_INVENTORY_INSTALLER_NAME, "installer_name"},
	{MVAR_INVENTORY_DEPLOYMENT_STATUS, "deployment_status"},
	{MVAR_INVENTORY_URL_A, "url_a"},
	{MVAR_INVENTORY_URL_B, "url_b"},
	{MVAR_INVENTORY_URL_C, "url_c"},
	{MVAR_INVENTORY_HOST_NETWORKS, "host_networks"},
	{MVAR_INVENTORY_HOST_NETMASK, "host_netmask"},
	{MVAR_INVENTORY_HOST_ROUTER, "host_router"},
	{MVAR_INVENTORY_OOB_IP, "oob_ip"},
	{MVAR_INVENTORY_OOB_NETMASK, "oob_netmask"},
	{MVAR_INVENTORY_OOB_ROUTER, "oob_router"},
	{MVAR_INVENTORY_HW_DATE_PURCHASE, "date_hw_purchase"},
	{MVAR_INVENTORY_HW_DATE_INSTALL, "date_hw_install"},
	{MVAR_INVENTORY_HW_DATE_EXPIRY, "date_hw_expiry"},
	{MVAR_INVENTORY_HW_DATE_DECOMM, "date_hw_decomm"},
	{MVAR_INVENTORY_SITE_ADDRESS_A, "site_address_a"},
	{MVAR_INVENTORY_SITE_ADDRESS_B, "site_address_b"},
	{MVAR_INVENTORY_SITE_ADDRESS_C, "site_address_c"},
	{MVAR_INVENTORY_SITE_CITY, "site_city"},
	{MVAR_INVENTORY_SITE_STATE, "site_state"},
	{MVAR_INVENTORY_SITE_COUNTRY, "site_country"},
	{MVAR_INVENTORY_SITE_ZIP, "site_zip"},
	{MVAR_INVENTORY_SITE_RACK, "site_rack"},
	{MVAR_INVENTORY_SITE_NOTES, "site_notes"},
	{MVAR_INVENTORY_POC_PRIMARY_NAME, "poc_1_name"},
	{MVAR_INVENTORY_POC_PRIMARY_EMAIL, "poc_1_email"},
	{MVAR_INVENTORY_POC_PRIMARY_PHONE_A, "poc_1_phone_a"},
	{MVAR_INVENTORY_POC_PRIMARY_PHONE_B, "poc_1_phone_b"},
	{MVAR_INVENTORY_POC_PRIMARY_CELL, "poc_1_cell"},
	{MVAR_INVENTORY_POC_PRIMARY_SCREEN, "poc_1_screen"},
	{MVAR_INVENTORY_POC_PRIMARY_NOTES, "poc_1_notes"},
	{MVAR_INVENTORY_POC_SECONDARY_NAME, "poc_2_name"},
	{MVAR_INVENTORY_POC_SECONDARY_EMAIL, "poc_2_email"},
	{MVAR_INVENTORY_POC_SECONDARY_PHONE_A, "poc_2_phone_a"},
	{MVAR_INVENTORY_POC_SECONDARY_PHONE_B, "poc_2_phone_b"},
	{MVAR_INVENTORY_POC_SECONDARY_CELL, "poc_2_cell"},
	{MVAR_INVENTORY_POC_SECONDARY_SCREEN, "poc_2_screen"},
	{MVAR_INVENTORY_POC_SECONDARY_NOTES, "poc_2_notes"},
	{NULL}
};

/******************************************************************************
 *                                                                            *
 * Function: get_action_value                                                 *
 *                                                                            *
 * Purpose: request action value by macro                                     *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 ******************************************************************************/
static int	get_action_value(const char *macro, zbx_uint64_t actionid, char **replace_to)
{
	int	ret = SUCCEED;

	if (0 == strcmp(macro, MVAR_ACTION_ID))
	{
		*replace_to = zbx_dsprintf(*replace_to, ZBX_FS_UI64, actionid);
	}
	else if (0 == strcmp(macro, MVAR_ACTION_NAME))
	{
		DB_RESULT	result;
		DB_ROW		row;

		result = DBselect("select name from actions where actionid=" ZBX_FS_UI64, actionid);

		if (NULL != (row = DBfetch(result)))
			*replace_to = zbx_strdup(*replace_to, row[0]);
		else
			ret = FAIL;

		DBfree_result(result);
	}

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: DBget_host_inventory_value                                       *
 *                                                                            *
 * Purpose: request host inventory value by itemid and field name             *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 ******************************************************************************/
static int	DBget_host_inventory_value(zbx_uint64_t itemid, char **replace_to, const char *field_name)
{
	const char	*__function_name = "DBget_host_inventory_value";

	DB_RESULT	result;
	DB_ROW		row;
	int		ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	result = DBselect(
			"select hi.%s"
			" from host_inventory hi,items i"
			" where hi.hostid=i.hostid"
				" and i.itemid=" ZBX_FS_UI64, field_name, itemid);

	if (NULL != (row = DBfetch(result)))
	{
		*replace_to = zbx_strdup(*replace_to, row[0]);
		ret = SUCCEED;
	}
	DBfree_result(result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: get_host_inventory                                               *
 *                                                                            *
 * Purpose: request host inventory value by macro and trigger                 *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 ******************************************************************************/
static int	get_host_inventory(const char *macro, const char *expression, char **replace_to,
		int N_functionid)
{
	int	i;

	for (i = 0; NULL != inventory_fields[i].macro; i++)
	{
		if (0 == strcmp(macro, inventory_fields[i].macro))
		{
			zbx_uint64_t	itemid;
			int		ret = FAIL;

			if (SUCCEED == get_N_itemid(expression, N_functionid, &itemid))
				ret = DBget_host_inventory_value(itemid, replace_to, inventory_fields[i].field_name);

			return ret;
		}
	}

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: get_host_inventory_by_itemid                                     *
 *                                                                            *
 * Purpose: request host inventory value by macro and itemid                  *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 ******************************************************************************/
static int	get_host_inventory_by_itemid(const char *macro, zbx_uint64_t itemid, char **replace_to)
{
	int	i;

	for (i = 0; NULL != inventory_fields[i].macro; i++)
	{
		if (0 == strcmp(macro, inventory_fields[i].macro))
			return DBget_host_inventory_value(itemid, replace_to, inventory_fields[i].field_name);
	}

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: compare_tags                                                     *
 *                                                                            *
 * Purpose: comparison function to sort tags by tag/value                     *
 *                                                                            *
 ******************************************************************************/
static int	compare_tags(const void *d1, const void *d2)
{
	int	ret;

	const zbx_tag_t	*tag1 = *(const zbx_tag_t **)d1;
	const zbx_tag_t	*tag2 = *(const zbx_tag_t **)d2;

	if (0 == (ret = zbx_strcmp_natural(tag1->tag, tag2->tag)))
		ret = zbx_strcmp_natural(tag1->value, tag2->value);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: get_event_tags                                                   *
 *                                                                            *
 * Purpose: format event tags string in format <tag1>[:<value1>], ...         *
 *                                                                            *
 * Parameters: event        [IN] the event                                    *
 *             replace_to - [OUT] replacement string                          *
 *                                                                            *
 ******************************************************************************/
static void	get_event_tags(const DB_EVENT *event, char **replace_to)
{
	size_t			replace_to_offset = 0, replace_to_alloc = 0;
	int			i;
	zbx_vector_ptr_t	tags;

	if (0 == event->tags.values_num)
	{
		*replace_to = zbx_strdup(*replace_to, "");
		return;
	}

	zbx_free(*replace_to);

	/* copy tags to temporary vector for sorting */

	zbx_vector_ptr_create(&tags);
	zbx_vector_ptr_reserve(&tags, event->tags.values_num);

	for (i = 0; i < event->tags.values_num; i++)
		zbx_vector_ptr_append(&tags, event->tags.values[i]);

	zbx_vector_ptr_sort(&tags, compare_tags);

	for (i = 0; i < tags.values_num; i++)
	{
		const zbx_tag_t	*tag = (const zbx_tag_t *)tags.values[i];

		if (0 != i)
			zbx_strcpy_alloc(replace_to, &replace_to_alloc, &replace_to_offset, ", ");

		zbx_strcpy_alloc(replace_to, &replace_to_alloc, &replace_to_offset, tag->tag);

		if ('\0' != *tag->value)
		{
			zbx_chrcpy_alloc(replace_to, &replace_to_alloc, &replace_to_offset, ':');
			zbx_strcpy_alloc(replace_to, &replace_to_alloc, &replace_to_offset, tag->value);
		}
	}

	zbx_vector_ptr_destroy(&tags);
}

/******************************************************************************
 *                                                                            *
 * Function: get_recovery_event_value                                         *
 *                                                                            *
 * Purpose: request recovery event value by macro                             *
 *                                                                            *
 ******************************************************************************/
static void	get_recovery_event_value(const char *macro, const DB_EVENT *r_event, char **replace_to)
{
	if (0 == strcmp(macro, MVAR_EVENT_RECOVERY_DATE))
	{
		*replace_to = zbx_strdup(*replace_to, zbx_date2str(r_event->clock));
	}
	else if (0 == strcmp(macro, MVAR_EVENT_RECOVERY_ID))
	{
		*replace_to = zbx_dsprintf(*replace_to, ZBX_FS_UI64, r_event->eventid);
	}
	else if (0 == strcmp(macro, MVAR_EVENT_RECOVERY_STATUS))
	{
		*replace_to = zbx_strdup(*replace_to,
				zbx_event_value_string(r_event->source, r_event->object, r_event->value));
	}
	else if (0 == strcmp(macro, MVAR_EVENT_RECOVERY_TIME))
	{
		*replace_to = zbx_strdup(*replace_to, zbx_time2str(r_event->clock));
	}
	else if (0 == strcmp(macro, MVAR_EVENT_RECOVERY_VALUE))
	{
		*replace_to = zbx_dsprintf(*replace_to, "%d", r_event->value);
	}
	else if (EVENT_SOURCE_TRIGGERS == r_event->source && 0 == strcmp(macro, MVAR_EVENT_RECOVERY_TAGS))
	{
		get_event_tags(r_event, replace_to);
	}
}

/******************************************************************************
 *                                                                            *
 * Function: get_event_value                                                  *
 *                                                                            *
 * Purpose: request event value by macro                                      *
 *                                                                            *
 ******************************************************************************/
static void	get_event_value(const char *macro, const DB_EVENT *event, char **replace_to)
{
	if (0 == strcmp(macro, MVAR_EVENT_AGE))
	{
		*replace_to = zbx_strdup(*replace_to, zbx_age2str(time(NULL) - event->clock));
	}
	else if (0 == strcmp(macro, MVAR_EVENT_DATE))
	{
		*replace_to = zbx_strdup(*replace_to, zbx_date2str(event->clock));
	}
	else if (0 == strcmp(macro, MVAR_EVENT_ID))
	{
		*replace_to = zbx_dsprintf(*replace_to, ZBX_FS_UI64, event->eventid);
	}
	else if (0 == strcmp(macro, MVAR_EVENT_TIME))
	{
		*replace_to = zbx_strdup(*replace_to, zbx_time2str(event->clock));
	}
	else if (EVENT_SOURCE_TRIGGERS == event->source || EVENT_SOURCE_INTERNAL == event->source)
	{
		if (0 == strcmp(macro, MVAR_EVENT_STATUS))
		{
			*replace_to = zbx_strdup(*replace_to,
					zbx_event_value_string(event->source, event->object, event->value));
		}
		else if (0 == strcmp(macro, MVAR_EVENT_VALUE))
		{
			*replace_to = zbx_dsprintf(*replace_to, "%d", event->value);
		}
		else if (EVENT_SOURCE_TRIGGERS == event->source)
		{
			if (0 == strcmp(macro, MVAR_EVENT_ACK_HISTORY))
			{
				get_event_ack_history(event, replace_to);
			}
			else if (0 == strcmp(macro, MVAR_EVENT_ACK_STATUS))
			{
				*replace_to = zbx_strdup(*replace_to, event->acknowledged ? "Yes" : "No");
			}
			else if (0 == strcmp(macro, MVAR_EVENT_TAGS))
			{
				get_event_tags(event, replace_to);
			}
		}
	}
}

/******************************************************************************
 *                                                                            *
 * Function: get_trigger_function_value                                       *
 *                                                                            *
 * Purpose: trying to evaluate a trigger function                             *
 *                                                                            *
 * Parameters: expression - [IN] string to parse                              *
 *             replace_to - [IN] the pointer to a result buffer               *
 *             bl         - [IN] the pointer to a left curly bracket          *
 *             br         - [OUT] the pointer to a next char, after a right   *
 *                          curly bracket                                     *
 *                                                                            *
 * Return value: (1) *replace_to = NULL - invalid function format;            *
 *                    'br' pointer remains unchanged                          *
 *               (2) *replace_to = "*UNKNOWN*" - invalid hostname, key or     *
 *                    function, or a function cannot be evaluated             *
 *               (3) *replace_to = "<value>" - a function successfully        *
 *                    evaluated                                               *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 * Comments: example: " {Zabbix server:{ITEM.KEY1}.last(0)} " to " 1.34 "     *
 *                      ^ - bl                             ^ - br             *
 *                                                                            *
 ******************************************************************************/
static void	get_trigger_function_value(const char *expression, char **replace_to, char *bl, char **br)
{
	char	*p, *host = NULL, *key = NULL;
	int	N_functionid, ret = FAIL;
	size_t	sz, par_l, par_r;

	p = bl + 1;

	if (0 == strncmp(p, MVAR_HOSTNAME, sz = sizeof(MVAR_HOSTNAME) - 2) ||
			0 == strncmp(p, MVAR_HOST_HOST, sz = sizeof(MVAR_HOST_HOST) - 2))
	{
		ret = SUCCEED;
	}

	if (SUCCEED == ret && ('}' == p[sz] || ('}' == p[sz + 1] && '1' <= p[sz] && p[sz] <= '9')))
	{
		N_functionid = ('}' == p[sz] ? 1 : p[sz] - '0');
		p += sz + ('}' == p[sz] ? 1 : 2);
		ret = DBget_trigger_value(expression, &host, N_functionid, ZBX_REQUEST_HOST_HOST);
	}
	else
		ret = parse_host(&p, &host);

	if (SUCCEED != ret || ':' != *p++)
		goto fail;

	if ((0 == strncmp(p, MVAR_ITEM_KEY, sz = sizeof(MVAR_ITEM_KEY) - 2) ||
				0 == strncmp(p, MVAR_TRIGGER_KEY, sz = sizeof(MVAR_TRIGGER_KEY) - 2)) &&
			('}' == p[sz] || ('}' == p[sz + 1] && '1' <= p[sz] && p[sz] <= '9')))
	{
		N_functionid = ('}' == p[sz] ? 1 : p[sz] - '0');
		p += sz + ('}' == p[sz] ? 1 : 2);
		ret = DBget_trigger_value(expression, &key, N_functionid, ZBX_REQUEST_ITEM_KEY_ORIG);
	}
	else
		ret = get_item_key(&p, &key);

	if (SUCCEED != ret || '.' != *p++)
		goto fail;

	if (SUCCEED != zbx_function_validate(p, &par_l, &par_r) || '}' != p[par_r + 1])
		goto fail;

	p[par_l] = '\0';
	p[par_r] = '\0';

	/* function 'evaluate_macro_function' requires 'replace_to' with size 'MAX_BUFFER_LEN' */
	*replace_to = zbx_realloc(*replace_to, MAX_BUFFER_LEN);

	if (NULL == host || NULL == key ||
			SUCCEED != evaluate_macro_function(*replace_to, host, key, p, p + par_l + 1))
	{
		zbx_strlcpy(*replace_to, STR_UNKNOWN_VARIABLE, MAX_BUFFER_LEN);
	}

	p[par_l] = '(';
	p[par_r] = ')';

	*br = p + par_r + 2;	/* point to the character after '}' */
fail:
	zbx_free(host);
	zbx_free(key);
}

/******************************************************************************
 *                                                                            *
 * Function: cache_trigger_hostids                                            *
 *                                                                            *
 * Purpose: cache host identifiers referenced by trigger expression           *
 *                                                                            *
 * Parameters: hostids             - [OUT] the host identifier cache          *
 *             expression          - [IN] the trigger expression              *
 *             recovery_expression - [IN] the trigger recovery expression     *
 *                                        (can be empty)                      *
 *                                                                            *
 ******************************************************************************/
static void	cache_trigger_hostids(zbx_vector_uint64_t *hostids, const char *expression,
		const char *recovery_expression)
{
	if (0 == hostids->values_num)
	{
		zbx_vector_uint64_t	functionids;

		zbx_vector_uint64_create(&functionids);
		get_functionids(&functionids, expression);
		get_functionids(&functionids, recovery_expression);
		DCget_hostids_by_functionids(&functionids, hostids);
		zbx_vector_uint64_destroy(&functionids);
	}
}

/******************************************************************************
 *                                                                            *
 * Function: cache_item_hostid                                                *
 *                                                                            *
 * Purpose: cache host identifier referenced by an item or a lld-rule         *
 *                                                                            *
 * Parameters: hostids - [OUT] the host identifier cache                      *
 *             itemid  - [IN]  the item identifier                            *
 *                                                                            *
 ******************************************************************************/
static void	cache_item_hostid(zbx_vector_uint64_t *hostids, zbx_uint64_t itemid)
{
	if (0 == hostids->values_num)
	{
		DC_ITEM	item;
		int	errcode;

		DCconfig_get_items_by_itemids(&item, &itemid, &errcode, 1);

		if (SUCCEED == errcode)
			zbx_vector_uint64_append(hostids, item.host.hostid);

		DCconfig_clean_items(&item, &errcode, 1);
	}
}

/******************************************************************************
 *                                                                            *
 * Function: get_trigger_severity_name                                        *
 *                                                                            *
 * Purpose: get trigger severity name                                         *
 *                                                                            *
 * Parameters: trigger    - [IN] a trigger data with priority field;          *
 *                               TRIGGER_SEVERITY_*                           *
 *             replace_to - [OUT] pointer to a buffer that will receive       *
 *                          a null-terminated trigger severity string         *
 *                                                                            *
 * Return value: upon successful completion return SUCCEED                    *
 *               otherwise FAIL                                               *
 *                                                                            *
 ******************************************************************************/
static int	get_trigger_severity_name(unsigned char priority, char **replace_to)
{
	zbx_config_t	cfg;

	if (TRIGGER_SEVERITY_COUNT <= priority)
		return FAIL;

	zbx_config_get(&cfg, ZBX_CONFIG_FLAGS_SEVERITY_NAME);

	*replace_to = zbx_strdup(*replace_to, cfg.severity_name[priority]);

	zbx_config_clean(&cfg);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: wrap_negative_double_suffix                                      *
 *                                                                            *
 * Purpose: wrap a replacement string that represents a negative number in    *
 *          parentheses (for instance, turn "-123.456M" into "(-123.456M)")   *
 *                                                                            *
 * Parameters: replace_to       - [IN/OUT] replacement string                 *
 *             replace_to_alloc - [IN/OUT] number of allocated bytes          *
 *                                                                            *
 ******************************************************************************/
static void	wrap_negative_double_suffix(char **replace_to, size_t *replace_to_alloc)
{
	size_t	replace_to_len;

	if ('-' != (*replace_to)[0])
		return;

	replace_to_len = strlen(*replace_to);

	if (NULL != replace_to_alloc && *replace_to_alloc >= replace_to_len + 3)
	{
		memmove(*replace_to + 1, *replace_to, replace_to_len);
	}
	else
	{
		char	*buffer;

		if (NULL != replace_to_alloc)
			*replace_to_alloc = replace_to_len + 3;

		buffer = zbx_malloc(NULL, replace_to_len + 3);

		memcpy(buffer + 1, *replace_to, replace_to_len);

		zbx_free(*replace_to);
		*replace_to = buffer;
	}

	(*replace_to)[0] = '(';
	(*replace_to)[replace_to_len + 1] = ')';
	(*replace_to)[replace_to_len + 2] = '\0';
}

/******************************************************************************
 *                                                                            *
 * Function: substitute_simple_macros                                         *
 *                                                                            *
 * Purpose: substitute simple macros in data string with real values          *
 *                                                                            *
 * Author: Eugene Grigorjev                                                   *
 *                                                                            *
 ******************************************************************************/
int	substitute_simple_macros(zbx_uint64_t *actionid, const DB_EVENT *event, const DB_EVENT *r_event,
		zbx_uint64_t *userid, zbx_uint64_t *hostid, DC_HOST *dc_host, DC_ITEM *dc_item, DB_ALERT *alert,
		char **data, int macro_type, char *error, int maxerrlen)
{
	const char		*__function_name = "substitute_simple_macros";

	char			*p, *bl, *br, c, *replace_to = NULL, sql[64];
	const char		*m;
	int			N_functionid, indexed_macro, require_numeric, ret, res = SUCCEED, pos = 0, func_macro,
				found, raw_value;
	size_t			data_alloc, data_len;
	DC_INTERFACE		interface;
	zbx_vector_uint64_t	hostids;
	zbx_token_t		token;

	if (NULL == data || NULL == *data || '\0' == **data)
	{
		zabbix_log(LOG_LEVEL_DEBUG, "In %s() data:EMPTY", __function_name);
		return res;
	}

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() data:'%s'", __function_name, *data);

	if (0 != (macro_type & MACRO_TYPE_TRIGGER_DESCRIPTION))
	{
		char	*expression;

		if (NULL != (expression = DCexpression_expand_user_macros(event->trigger.expression, NULL)))
		{
			expand_trigger_description_constants(data, expression);
			zbx_free(expression);
		}
	}

	if (SUCCEED != zbx_token_find(*data, pos, &token))
		return res;

	zbx_vector_uint64_create(&hostids);

	data_alloc = data_len = strlen(*data) + 1;

	for (found = SUCCEED; SUCCEED == res && SUCCEED == found; found = zbx_token_find(*data, pos, &token))
	{
		indexed_macro = 0;
		func_macro = 0;
		require_numeric = 0;
		/* temporarily reset function id to 0 before parsing possible indexed macros */
		N_functionid = 0;
		raw_value = 0;

		switch (token.type)
		{
			case ZBX_TOKEN_OBJECTID:
			case ZBX_TOKEN_LLD_MACRO:
				/* neither lld or {123123} macros are processed by this function, skip them */
				pos = token.token.r + 1;
				continue;
			case ZBX_TOKEN_MACRO:
				p = *data + token.token.r - 1;
				m = *data + token.token.l;
				if ('1' <= *p && *p <= '9')
					N_functionid = *p - '0';
				break;
			case ZBX_TOKEN_FUNC_MACRO:
				raw_value = 1;
				p = *data + token.data.func_macro.macro.r - 1;
				m = *data + token.data.func_macro.macro.l;
				if ('1' <= *p && *p <= '9')
					N_functionid = *p - '0';
				break;
			default:
				m = *data + token.token.l;
		}

		bl = *data + token.token.l;
		br = *data + token.token.r;

		if (0 != N_functionid)
		{
			int	i, diff;

			for (i = 0; NULL != ex_macros[i]; i++)
			{
				diff = zbx_mismatch(ex_macros[i], m);

				if ('}' == ex_macros[i][diff] && '}' == m[diff + 1])
				{
					indexed_macro = 1;
					m = ex_macros[i];
					break;
				}
			}
		}

		/* reset functionid to its default value (first function) if this is not indexed macro */
		if (0 == indexed_macro)
			N_functionid = 1;

		c = *++br;
		*br = '\0';

		ret = SUCCEED;

		if (0 != (macro_type & (MACRO_TYPE_MESSAGE_NORMAL | MACRO_TYPE_MESSAGE_RECOVERY)))
		{
			const DB_EVENT	*c_event;

			c_event = ((NULL != r_event) ? r_event : event);

			if (EVENT_SOURCE_TRIGGERS == c_event->source)
			{
				if (0 == strncmp(m, "{$", 2))	/* user defined macros */
				{
					cache_trigger_hostids(&hostids, c_event->trigger.expression,
							c_event->trigger.recovery_expression);
					DCget_user_macro(hostids.values, hostids.values_num, m, &replace_to);
					pos = token.token.r;
				}
				else if (0 == strncmp(m, MVAR_ACTION, ZBX_CONST_STRLEN(MVAR_ACTION)))
				{
					ret = get_action_value(m, *actionid, &replace_to);
				}
				else if (0 == strcmp(m, MVAR_DATE))
				{
					replace_to = zbx_strdup(replace_to, zbx_date2str(time(NULL)));
				}
				else if (0 == strcmp(m, MVAR_ESC_HISTORY))
				{
					get_escalation_history(*actionid, event, r_event, &replace_to);
				}
				else if (0 == strncmp(m, MVAR_EVENT_RECOVERY, ZBX_CONST_STRLEN(MVAR_EVENT_RECOVERY)))
				{
					if (NULL != r_event)
						get_recovery_event_value(m, r_event, &replace_to);
				}
				else if (0 == strncmp(m, MVAR_EVENT, ZBX_CONST_STRLEN(MVAR_EVENT)))
				{
					get_event_value(m, event, &replace_to);
				}
				else if (0 == strcmp(m, MVAR_HOST_HOST) || 0 == strcmp(m, MVAR_HOSTNAME))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_HOST_HOST);
				}
				else if (0 == strcmp(m, MVAR_HOST_NAME))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_HOST_NAME);
				}
				else if (0 == strcmp(m, MVAR_HOST_DESCRIPTION))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_HOST_DESCRIPTION);
				}
				else if (0 == strcmp(m, MVAR_HOST_IP) || 0 == strcmp(m, MVAR_IPADDRESS))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_HOST_IP);
				}
				else if (0 == strcmp(m, MVAR_HOST_DNS))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_HOST_DNS);
				}
				else if (0 == strcmp(m, MVAR_HOST_CONN))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_HOST_CONN);
				}
				else if (0 == strcmp(m, MVAR_HOST_PORT))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_HOST_PORT);
				}
				else if (0 == strncmp(m, MVAR_INVENTORY, ZBX_CONST_STRLEN(MVAR_INVENTORY)) ||
						0 == strncmp(m, MVAR_PROFILE, ZBX_CONST_STRLEN(MVAR_PROFILE)))
				{
					ret = get_host_inventory(m, c_event->trigger.expression, &replace_to,
							N_functionid);
				}
				else if (0 == strcmp(m, MVAR_ITEM_DESCRIPTION))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_ITEM_DESCRIPTION);
				}
				else if (0 == strcmp(m, MVAR_ITEM_ID))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_ITEM_ID);
				}
				else if (0 == strcmp(m, MVAR_ITEM_KEY) || 0 == strcmp(m, MVAR_TRIGGER_KEY))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_ITEM_KEY);
				}
				else if (0 == strcmp(m, MVAR_ITEM_KEY_ORIG))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_ITEM_KEY_ORIG);
				}
				else if (0 == strncmp(m, MVAR_ITEM_LASTVALUE, ZBX_CONST_STRLEN(MVAR_ITEM_LASTVALUE)))
				{
					func_macro = 1;
					ret = DBitem_lastvalue(c_event->trigger.expression, &replace_to, N_functionid,
							raw_value);
				}
				else if (0 == strcmp(m, MVAR_ITEM_LOG_AGE))
				{
					ret = get_history_log_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_ITEM_LOG_AGE, c_event->clock,
							c_event->ns);
				}
				else if (0 == strcmp(m, MVAR_ITEM_LOG_DATE))
				{
					ret = get_history_log_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_ITEM_LOG_DATE, c_event->clock,
							c_event->ns);
				}
				else if (0 == strcmp(m, MVAR_ITEM_LOG_EVENTID))
				{
					ret = get_history_log_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_ITEM_LOG_EVENTID, c_event->clock,
							c_event->ns);
				}
				else if (0 == strcmp(m, MVAR_ITEM_LOG_NSEVERITY))
				{
					ret = get_history_log_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_ITEM_LOG_NSEVERITY, c_event->clock,
							c_event->ns);
				}
				else if (0 == strcmp(m, MVAR_ITEM_LOG_SEVERITY))
				{
					ret = get_history_log_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_ITEM_LOG_SEVERITY, c_event->clock,
							c_event->ns);
				}
				else if (0 == strcmp(m, MVAR_ITEM_LOG_SOURCE))
				{
					ret = get_history_log_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_ITEM_LOG_SOURCE, c_event->clock,
							c_event->ns);
				}
				else if (0 == strcmp(m, MVAR_ITEM_LOG_TIME))
				{
					ret = get_history_log_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_ITEM_LOG_TIME, c_event->clock,
							c_event->ns);
				}
				else if (0 == strcmp(m, MVAR_ITEM_NAME))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_ITEM_NAME);
				}
				else if (0 == strcmp(m, MVAR_ITEM_NAME_ORIG))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_ITEM_NAME_ORIG);
				}
				else if (0 == strncmp(m, MVAR_ITEM_VALUE, ZBX_CONST_STRLEN(MVAR_ITEM_VALUE)))
				{
					func_macro = 1;
					ret = DBitem_value(c_event->trigger.expression, &replace_to, N_functionid,
							c_event->clock, c_event->ns, raw_value);
				}
				else if (0 == strcmp(m, MVAR_PROXY_NAME))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_PROXY_NAME);
				}
				else if (0 == strcmp(m, MVAR_PROXY_DESCRIPTION))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_PROXY_DESCRIPTION);
				}
				else if (0 == strcmp(m, MVAR_TIME))
				{
					replace_to = zbx_strdup(replace_to, zbx_time2str(time(NULL)));
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_DESCRIPTION) ||
						0 == strcmp(m, MVAR_TRIGGER_COMMENT))
				{
					replace_to = zbx_strdup(replace_to, c_event->trigger.comments);
					substitute_simple_macros(NULL, c_event, NULL, NULL, NULL, NULL, NULL, NULL,
							&replace_to, MACRO_TYPE_TRIGGER_COMMENTS, error,
							maxerrlen);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_EVENTS_ACK))
				{
					ret = DBget_trigger_event_count(c_event->objectid, &replace_to, 0, 1);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_EVENTS_PROBLEM_ACK))
				{
					ret = DBget_trigger_event_count(c_event->objectid, &replace_to, 1, 1);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_EVENTS_PROBLEM_UNACK))
				{
					ret = DBget_trigger_event_count(c_event->objectid, &replace_to, 1, 0);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_EVENTS_UNACK))
				{
					ret = DBget_trigger_event_count(c_event->objectid, &replace_to, 0, 0);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_EXPRESSION))
				{
					replace_to = zbx_strdup(replace_to, c_event->trigger.expression);
					DCexpand_trigger_expression(&replace_to);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_EXPRESSION_RECOVERY))
				{
					if (TRIGGER_RECOVERY_MODE_RECOVERY_EXPRESSION == c_event->trigger.recovery_mode)
					{
						replace_to = zbx_strdup(replace_to,
								c_event->trigger.recovery_expression);
						DCexpand_trigger_expression(&replace_to);
					}
					else
						replace_to = zbx_strdup(replace_to, "");
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_HOSTGROUP_NAME))
				{
					ret = DBget_trigger_hostgroup_name(c_event->objectid, userid, &replace_to);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_ID))
				{
					replace_to = zbx_dsprintf(replace_to, ZBX_FS_UI64, c_event->objectid);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_NAME))
				{
					replace_to = zbx_strdup(replace_to, c_event->trigger.description);
					substitute_simple_macros(NULL, c_event, NULL, NULL, NULL, NULL, NULL, NULL,
							&replace_to, MACRO_TYPE_TRIGGER_DESCRIPTION, error,
							maxerrlen);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_NAME_ORIG))
				{
					replace_to = zbx_strdup(replace_to, c_event->trigger.description);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_NSEVERITY))
				{
					replace_to = zbx_dsprintf(replace_to, "%d", (int)c_event->trigger.priority);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_STATUS) || 0 == strcmp(m, MVAR_STATUS))
				{
					replace_to = zbx_strdup(replace_to, zbx_trigger_value_string(c_event->value));
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_SEVERITY))
				{
					ret = get_trigger_severity_name(c_event->trigger.priority, &replace_to);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_TEMPLATE_NAME))
				{
					ret = DBget_trigger_template_name(c_event->objectid, userid, &replace_to);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_URL))
				{
					replace_to = zbx_strdup(replace_to, c_event->trigger.url);
					substitute_simple_macros(NULL, c_event, NULL, NULL, NULL, NULL, NULL, NULL,
							&replace_to, MACRO_TYPE_TRIGGER_URL, error, maxerrlen);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_VALUE))
				{
					replace_to = zbx_dsprintf(replace_to, "%d", c_event->value);
				}
				else
				{
					*br = c;
					get_trigger_function_value(c_event->trigger.expression, &replace_to, bl, &br);
					c = *br;
					*br = '\0';
				}
			}
			else if (EVENT_SOURCE_INTERNAL == c_event->source && EVENT_OBJECT_TRIGGER == c_event->object)
			{
				if (0 == strncmp(m, "{$", 2))	/* user defined macros */
				{
					cache_trigger_hostids(&hostids, c_event->trigger.expression,
							c_event->trigger.recovery_expression);
					DCget_user_macro(hostids.values, hostids.values_num, m, &replace_to);
					pos = token.token.r;
				}
				else if (0 == strncmp(m, MVAR_ACTION, ZBX_CONST_STRLEN(MVAR_ACTION)))
				{
					ret = get_action_value(m, *actionid, &replace_to);
				}
				else if (0 == strcmp(m, MVAR_DATE))
				{
					replace_to = zbx_strdup(replace_to, zbx_date2str(time(NULL)));
				}
				else if (0 == strcmp(m, MVAR_ESC_HISTORY))
				{
					get_escalation_history(*actionid, event, r_event, &replace_to);
				}
				else if (0 == strncmp(m, MVAR_EVENT_RECOVERY, ZBX_CONST_STRLEN(MVAR_EVENT_RECOVERY)))
				{
					if (NULL != r_event)
						get_recovery_event_value(m, r_event, &replace_to);
				}
				else if (0 == strncmp(m, MVAR_EVENT, ZBX_CONST_STRLEN(MVAR_EVENT)))
				{
					get_event_value(m, event, &replace_to);
				}
				else if (0 == strcmp(m, MVAR_HOST_HOST) || 0 == strcmp(m, MVAR_HOSTNAME))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_HOST_HOST);
				}
				else if (0 == strcmp(m, MVAR_HOST_NAME))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_HOST_NAME);
				}
				else if (0 == strcmp(m, MVAR_HOST_DESCRIPTION))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_HOST_DESCRIPTION);
				}
				else if (0 == strcmp(m, MVAR_HOST_IP) || 0 == strcmp(m, MVAR_IPADDRESS))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_HOST_IP);
				}
				else if (0 == strcmp(m, MVAR_HOST_DNS))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_HOST_DNS);
				}
				else if (0 == strcmp(m, MVAR_HOST_CONN))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_HOST_CONN);
				}
				else if (0 == strcmp(m, MVAR_HOST_PORT))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_HOST_PORT);
				}
				else if (0 == strncmp(m, MVAR_INVENTORY, ZBX_CONST_STRLEN(MVAR_INVENTORY)) ||
						0 == strncmp(m, MVAR_PROFILE, ZBX_CONST_STRLEN(MVAR_PROFILE)))
				{
					ret = get_host_inventory(m, c_event->trigger.expression, &replace_to,
							N_functionid);
				}
				else if (0 == strcmp(m, MVAR_ITEM_DESCRIPTION))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_ITEM_DESCRIPTION);
				}
				else if (0 == strcmp(m, MVAR_ITEM_ID))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_ITEM_ID);
				}
				else if (0 == strcmp(m, MVAR_ITEM_KEY) || 0 == strcmp(m, MVAR_TRIGGER_KEY))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_ITEM_KEY);
				}
				else if (0 == strcmp(m, MVAR_ITEM_KEY_ORIG))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_ITEM_KEY_ORIG);
				}
				else if (0 == strcmp(m, MVAR_ITEM_NAME))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_ITEM_NAME);
				}
				else if (0 == strcmp(m, MVAR_ITEM_NAME_ORIG))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_ITEM_NAME_ORIG);
				}
				else if (0 == strcmp(m, MVAR_PROXY_NAME))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_PROXY_NAME);
				}
				else if (0 == strcmp(m, MVAR_PROXY_DESCRIPTION))
				{
					ret = DBget_trigger_value(c_event->trigger.expression, &replace_to,
							N_functionid, ZBX_REQUEST_PROXY_DESCRIPTION);
				}
				else if (0 == strcmp(m, MVAR_TIME))
				{
					replace_to = zbx_strdup(replace_to, zbx_time2str(time(NULL)));
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_DESCRIPTION) ||
						0 == strcmp(m, MVAR_TRIGGER_COMMENT))
				{
					replace_to = zbx_strdup(replace_to, c_event->trigger.comments);
					substitute_simple_macros(NULL, c_event, NULL, NULL, NULL, NULL, NULL, NULL,
							&replace_to, MACRO_TYPE_TRIGGER_COMMENTS, error,
							maxerrlen);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_EXPRESSION))
				{
					replace_to = zbx_strdup(replace_to, c_event->trigger.expression);
					DCexpand_trigger_expression(&replace_to);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_EXPRESSION_RECOVERY))
				{
					if (TRIGGER_RECOVERY_MODE_RECOVERY_EXPRESSION == c_event->trigger.recovery_mode)
					{
						replace_to = zbx_strdup(replace_to,
								c_event->trigger.recovery_expression);
						DCexpand_trigger_expression(&replace_to);
					}
					else
						replace_to = zbx_strdup(replace_to, "");
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_HOSTGROUP_NAME))
				{
					ret = DBget_trigger_hostgroup_name(c_event->objectid, userid, &replace_to);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_ID))
				{
					replace_to = zbx_dsprintf(replace_to, ZBX_FS_UI64, c_event->objectid);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_NAME))
				{
					replace_to = zbx_strdup(replace_to, c_event->trigger.description);
					substitute_simple_macros(NULL, c_event, NULL, NULL, NULL, NULL, NULL, NULL,
							&replace_to, MACRO_TYPE_TRIGGER_DESCRIPTION, error,
							maxerrlen);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_NAME_ORIG))
				{
					replace_to = zbx_strdup(replace_to, c_event->trigger.description);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_NSEVERITY))
				{
					replace_to = zbx_dsprintf(replace_to, "%d", (int)c_event->trigger.priority);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_SEVERITY))
				{
					ret = get_trigger_severity_name(c_event->trigger.priority, &replace_to);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_STATE))
				{
					replace_to = zbx_strdup(replace_to, zbx_trigger_state_string(c_event->value));
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_TEMPLATE_NAME))
				{
					ret = DBget_trigger_template_name(c_event->objectid, userid, &replace_to);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_URL))
				{
					replace_to = zbx_strdup(replace_to, c_event->trigger.url);
					substitute_simple_macros(NULL, c_event, NULL, NULL, NULL, NULL, NULL, NULL,
							&replace_to, MACRO_TYPE_TRIGGER_URL, error, maxerrlen);
				}
			}
			else if (0 == indexed_macro && EVENT_SOURCE_DISCOVERY == c_event->source)
			{
				if (0 == strncmp(m, "{$", 2))	/* user defined macros */
				{
					DCget_user_macro(NULL, 0, m, &replace_to);
					pos = token.token.r;
				}
				else if (0 == strncmp(m, MVAR_ACTION, ZBX_CONST_STRLEN(MVAR_ACTION)))
				{
					ret = get_action_value(m, *actionid, &replace_to);
				}
				else if (0 == strcmp(m, MVAR_DATE))
				{
					replace_to = zbx_strdup(replace_to, zbx_date2str(time(NULL)));
				}
				else if (0 == strncmp(m, MVAR_EVENT, ZBX_CONST_STRLEN(MVAR_EVENT)))
				{
					get_event_value(m, event, &replace_to);
				}
				else if (0 == strcmp(m, MVAR_DISCOVERY_DEVICE_IPADDRESS))
				{
					ret = DBget_dhost_value_by_event(c_event, &replace_to, "s.ip");
				}
				else if (0 == strcmp(m, MVAR_DISCOVERY_DEVICE_DNS))
				{
					ret = DBget_dhost_value_by_event(c_event, &replace_to, "s.dns");
				}
				else if (0 == strcmp(m, MVAR_DISCOVERY_DEVICE_STATUS))
				{
					if (SUCCEED == (ret = DBget_dhost_value_by_event(c_event, &replace_to,
							"h.status")))
					{
						replace_to = zbx_strdup(replace_to,
								DOBJECT_STATUS_UP == atoi(replace_to) ? "UP" : "DOWN");
					}
				}
				else if (0 == strcmp(m, MVAR_DISCOVERY_DEVICE_UPTIME))
				{
					zbx_snprintf(sql, sizeof(sql),
							"case when h.status=%d then h.lastup else h.lastdown end",
							DOBJECT_STATUS_UP);
					if (SUCCEED == (ret = DBget_dhost_value_by_event(c_event, &replace_to, sql)))
					{
						replace_to = zbx_strdup(replace_to,
								zbx_age2str(time(NULL) - atoi(replace_to)));
					}
				}
				else if (0 == strcmp(m, MVAR_DISCOVERY_RULE_NAME))
				{
					ret = DBget_drule_value_by_event(c_event, &replace_to, "name");
				}
				else if (0 == strcmp(m, MVAR_DISCOVERY_SERVICE_NAME))
				{
					if (SUCCEED == (ret = DBget_dservice_value_by_event(c_event, &replace_to,
							"s.type")))
					{
						replace_to = zbx_strdup(replace_to,
								zbx_dservice_type_string(atoi(replace_to)));
					}
				}
				else if (0 == strcmp(m, MVAR_DISCOVERY_SERVICE_PORT))
				{
					ret = DBget_dservice_value_by_event(c_event, &replace_to, "s.port");
				}
				else if (0 == strcmp(m, MVAR_DISCOVERY_SERVICE_STATUS))
				{
					if (SUCCEED == (ret = DBget_dservice_value_by_event(c_event, &replace_to,
							"s.status")))
					{
						replace_to = zbx_strdup(replace_to,
								DOBJECT_STATUS_UP == atoi(replace_to) ? "UP" : "DOWN");
					}
				}
				else if (0 == strcmp(m, MVAR_DISCOVERY_SERVICE_UPTIME))
				{
					zbx_snprintf(sql, sizeof(sql),
							"case when s.status=%d then s.lastup else s.lastdown end",
							DOBJECT_STATUS_UP);
					if (SUCCEED == (ret = DBget_dservice_value_by_event(c_event, &replace_to, sql)))
					{
						replace_to = zbx_strdup(replace_to,
								zbx_age2str(time(NULL) - atoi(replace_to)));
					}
				}
				else if (0 == strcmp(m, MVAR_PROXY_NAME))
				{
					if (SUCCEED == (ret = DBget_dhost_value_by_event(c_event, &replace_to,
							"r.proxy_hostid")))
					{
						zbx_uint64_t	proxy_hostid;

						ZBX_DBROW2UINT64(proxy_hostid, replace_to);

						if (0 == proxy_hostid)
							replace_to = zbx_strdup(replace_to, "");
						else
							ret = DBget_host_value(proxy_hostid, &replace_to, "host");
					}
				}
				else if (0 == strcmp(m, MVAR_PROXY_DESCRIPTION))
				{
					if (SUCCEED == (ret = DBget_dhost_value_by_event(c_event, &replace_to,
							"r.proxy_hostid")))
					{
						zbx_uint64_t	proxy_hostid;

						ZBX_DBROW2UINT64(proxy_hostid, replace_to);

						if (0 == proxy_hostid)
						{
							replace_to = zbx_strdup(replace_to, "");
						}
						else
						{
							ret = DBget_host_value(proxy_hostid, &replace_to,
									"description");
						}
					}
				}
				else if (0 == strcmp(m, MVAR_TIME))
				{
					replace_to = zbx_strdup(replace_to, zbx_time2str(time(NULL)));
				}
			}
			else if (0 == indexed_macro && EVENT_SOURCE_AUTO_REGISTRATION == c_event->source)
			{
				if (0 == strncmp(m, "{$", 2))	/* user defined macros */
				{
					DCget_user_macro(NULL, 0, m, &replace_to);
					pos = token.token.r;
				}
				else if (0 == strncmp(m, MVAR_ACTION, ZBX_CONST_STRLEN(MVAR_ACTION)))
				{
					ret = get_action_value(m, *actionid, &replace_to);
				}
				else if (0 == strcmp(m, MVAR_DATE))
				{
					replace_to = zbx_strdup(replace_to, zbx_date2str(time(NULL)));
				}
				else if (0 == strncmp(m, MVAR_EVENT, ZBX_CONST_STRLEN(MVAR_EVENT)))
				{
					get_event_value(m, event, &replace_to);
				}
				else if (0 == strcmp(m, MVAR_HOST_METADATA))
				{
					ret = get_autoreg_value_by_event(c_event, &replace_to, "host_metadata");
				}
				else if (0 == strcmp(m, MVAR_HOST_HOST))
				{
					ret = get_autoreg_value_by_event(c_event, &replace_to, "host");
				}
				else if (0 == strcmp(m, MVAR_HOST_IP) || 0 == strcmp(m, MVAR_IPADDRESS))
				{
					ret = get_autoreg_value_by_event(c_event, &replace_to, "listen_ip");
				}
				else if (0 == strcmp(m, MVAR_HOST_PORT))
				{
					ret = get_autoreg_value_by_event(c_event, &replace_to, "listen_port");
				}
				else if (0 == strcmp(m, MVAR_PROXY_NAME))
				{
					if (SUCCEED == (ret = get_autoreg_value_by_event(c_event, &replace_to,
							"proxy_hostid")))
					{
						zbx_uint64_t	proxy_hostid;

						ZBX_DBROW2UINT64(proxy_hostid, replace_to);

						if (0 == proxy_hostid)
							replace_to = zbx_strdup(replace_to, "");
						else
							ret = DBget_host_value(proxy_hostid, &replace_to, "host");
					}
				}
				else if (0 == strcmp(m, MVAR_PROXY_DESCRIPTION))
				{
					if (SUCCEED == (ret = get_autoreg_value_by_event(c_event, &replace_to,
							"proxy_hostid")))
					{
						zbx_uint64_t	proxy_hostid;

						ZBX_DBROW2UINT64(proxy_hostid, replace_to);

						if (0 == proxy_hostid)
						{
							replace_to = zbx_strdup(replace_to, "");
						}
						else
						{
							ret = DBget_host_value(proxy_hostid, &replace_to,
									"description");
						}
					}
				}
				else if (0 == strcmp(m, MVAR_TIME))
				{
					replace_to = zbx_strdup(replace_to, zbx_time2str(time(NULL)));
				}
			}
			else if (0 == indexed_macro && EVENT_SOURCE_INTERNAL == c_event->source &&
					EVENT_OBJECT_ITEM == c_event->object)
			{
				if (0 == strncmp(m, "{$", 2))	/* user defined macros */
				{
					cache_item_hostid(&hostids, c_event->objectid);
					DCget_user_macro(hostids.values, hostids.values_num, m, &replace_to);
					pos = token.token.r;
				}
				else if (0 == strncmp(m, MVAR_ACTION, ZBX_CONST_STRLEN(MVAR_ACTION)))
				{
					ret = get_action_value(m, *actionid, &replace_to);
				}
				else if (0 == strcmp(m, MVAR_DATE))
				{
					replace_to = zbx_strdup(replace_to, zbx_date2str(time(NULL)));
				}
				else if (0 == strcmp(m, MVAR_ESC_HISTORY))
				{
					get_escalation_history(*actionid, event, r_event, &replace_to);
				}
				else if (0 == strncmp(m, MVAR_EVENT_RECOVERY, ZBX_CONST_STRLEN(MVAR_EVENT_RECOVERY)))
				{
					if (NULL != r_event)
						get_recovery_event_value(m, r_event, &replace_to);
				}
				else if (0 == strncmp(m, MVAR_EVENT, ZBX_CONST_STRLEN(MVAR_EVENT)))
				{
					get_event_value(m, event, &replace_to);
				}
				else if (0 == strcmp(m, MVAR_HOST_HOST) || 0 == strcmp(m, MVAR_HOSTNAME))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to, ZBX_REQUEST_HOST_HOST);
				}
				else if (0 == strcmp(m, MVAR_HOST_NAME))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to, ZBX_REQUEST_HOST_NAME);
				}
				else if (0 == strcmp(m, MVAR_HOST_DESCRIPTION))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to,
							ZBX_REQUEST_HOST_DESCRIPTION);
				}
				else if (0 == strcmp(m, MVAR_HOST_IP) || 0 == strcmp(m, MVAR_IPADDRESS))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to, ZBX_REQUEST_HOST_IP);
				}
				else if (0 == strcmp(m, MVAR_HOST_DNS))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to, ZBX_REQUEST_HOST_DNS);
				}
				else if (0 == strcmp(m, MVAR_HOST_CONN))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to, ZBX_REQUEST_HOST_CONN);
				}
				else if (0 == strcmp(m, MVAR_HOST_PORT))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to, ZBX_REQUEST_HOST_PORT);
				}
				else if (0 == strncmp(m, MVAR_INVENTORY, ZBX_CONST_STRLEN(MVAR_INVENTORY)) ||
						0 == strncmp(m, MVAR_PROFILE, ZBX_CONST_STRLEN(MVAR_PROFILE)))
				{
					ret = get_host_inventory_by_itemid(m, c_event->objectid, &replace_to);
				}
				else if (0 == strcmp(m, MVAR_ITEM_DESCRIPTION))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to,
							ZBX_REQUEST_ITEM_DESCRIPTION);
				}
				else if (0 == strcmp(m, MVAR_ITEM_ID))
				{
					replace_to = zbx_dsprintf(replace_to, ZBX_FS_UI64, c_event->objectid);
				}
				else if (0 == strcmp(m, MVAR_ITEM_KEY) || 0 == strcmp(m, MVAR_TRIGGER_KEY))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to, ZBX_REQUEST_ITEM_KEY);
				}
				else if (0 == strcmp(m, MVAR_ITEM_KEY_ORIG))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to,
							ZBX_REQUEST_ITEM_KEY_ORIG);
				}
				else if (0 == strcmp(m, MVAR_ITEM_NAME))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to, ZBX_REQUEST_ITEM_NAME);
				}
				else if (0 == strcmp(m, MVAR_ITEM_NAME_ORIG))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to,
							ZBX_REQUEST_ITEM_NAME_ORIG);
				}
				else if (0 == strcmp(m, MVAR_ITEM_STATE))
				{
					replace_to = zbx_strdup(replace_to, zbx_item_state_string(c_event->value));
				}
				else if (0 == strcmp(m, MVAR_PROXY_NAME))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to, ZBX_REQUEST_PROXY_NAME);
				}
				else if (0 == strcmp(m, MVAR_PROXY_DESCRIPTION))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to,
							ZBX_REQUEST_PROXY_DESCRIPTION);
				}
				else if (0 == strcmp(m, MVAR_TIME))
				{
					replace_to = zbx_strdup(replace_to, zbx_time2str(time(NULL)));
				}
			}
			else if (0 == indexed_macro && EVENT_SOURCE_INTERNAL == c_event->source &&
					EVENT_OBJECT_LLDRULE == c_event->object)
			{
				if (0 == strncmp(m, "{$", 2))	/* user defined macros */
				{
					cache_item_hostid(&hostids, c_event->objectid);
					DCget_user_macro(hostids.values, hostids.values_num, m, &replace_to);
					pos = token.token.r;
				}
				else if (0 == strncmp(m, MVAR_ACTION, ZBX_CONST_STRLEN(MVAR_ACTION)))
				{
					ret = get_action_value(m, *actionid, &replace_to);
				}
				else if (0 == strcmp(m, MVAR_DATE))
				{
					replace_to = zbx_strdup(replace_to, zbx_date2str(time(NULL)));
				}
				else if (0 == strcmp(m, MVAR_ESC_HISTORY))
				{
					get_escalation_history(*actionid, event, r_event, &replace_to);
				}
				else if (0 == strncmp(m, MVAR_EVENT_RECOVERY, ZBX_CONST_STRLEN(MVAR_EVENT_RECOVERY)))
				{
					if (NULL != r_event)
						get_recovery_event_value(m, r_event, &replace_to);
				}
				else if (0 == strncmp(m, MVAR_EVENT, ZBX_CONST_STRLEN(MVAR_EVENT)))
				{
					get_event_value(m, event, &replace_to);
				}
				else if (0 == strcmp(m, MVAR_HOST_HOST) || 0 == strcmp(m, MVAR_HOSTNAME))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to, ZBX_REQUEST_HOST_HOST);
				}
				else if (0 == strcmp(m, MVAR_HOST_NAME))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to, ZBX_REQUEST_HOST_NAME);
				}
				else if (0 == strcmp(m, MVAR_HOST_DESCRIPTION))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to,
							ZBX_REQUEST_HOST_DESCRIPTION);
				}
				else if (0 == strcmp(m, MVAR_HOST_IP) || 0 == strcmp(m, MVAR_IPADDRESS))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to, ZBX_REQUEST_HOST_IP);
				}
				else if (0 == strcmp(m, MVAR_HOST_DNS))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to, ZBX_REQUEST_HOST_DNS);
				}
				else if (0 == strcmp(m, MVAR_HOST_CONN))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to, ZBX_REQUEST_HOST_CONN);
				}
				else if (0 == strcmp(m, MVAR_HOST_PORT))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to, ZBX_REQUEST_HOST_PORT);
				}
				else if (0 == strncmp(m, MVAR_INVENTORY, ZBX_CONST_STRLEN(MVAR_INVENTORY)) ||
						0 == strncmp(m, MVAR_PROFILE, ZBX_CONST_STRLEN(MVAR_PROFILE)))
				{
					ret = get_host_inventory_by_itemid(m, c_event->objectid, &replace_to);
				}
				else if (0 == strcmp(m, MVAR_LLDRULE_DESCRIPTION))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to,
							ZBX_REQUEST_ITEM_DESCRIPTION);
				}
				else if (0 == strcmp(m, MVAR_LLDRULE_ID))
				{
					replace_to = zbx_dsprintf(replace_to, ZBX_FS_UI64, c_event->objectid);
				}
				else if (0 == strcmp(m, MVAR_LLDRULE_KEY))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to, ZBX_REQUEST_ITEM_KEY);
				}
				else if (0 == strcmp(m, MVAR_LLDRULE_KEY_ORIG))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to,
							ZBX_REQUEST_ITEM_KEY_ORIG);
				}
				else if (0 == strcmp(m, MVAR_LLDRULE_NAME))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to, ZBX_REQUEST_ITEM_NAME);
				}
				else if (0 == strcmp(m, MVAR_LLDRULE_NAME_ORIG))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to,
							ZBX_REQUEST_ITEM_NAME_ORIG);
				}
				else if (0 == strcmp(m, MVAR_LLDRULE_STATE))
				{
					replace_to = zbx_strdup(replace_to, zbx_item_state_string(c_event->value));
				}
				else if (0 == strcmp(m, MVAR_PROXY_NAME))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to, ZBX_REQUEST_PROXY_NAME);
				}
				else if (0 == strcmp(m, MVAR_PROXY_DESCRIPTION))
				{
					ret = DBget_item_value(c_event->objectid, &replace_to,
							ZBX_REQUEST_PROXY_DESCRIPTION);
				}
				else if (0 == strcmp(m, MVAR_TIME))
				{
					replace_to = zbx_strdup(replace_to, zbx_time2str(time(NULL)));
				}
			}
		}
		else if (0 != (macro_type & (MACRO_TYPE_TRIGGER_DESCRIPTION | MACRO_TYPE_TRIGGER_COMMENTS)))
		{
			if (EVENT_OBJECT_TRIGGER == event->object)
			{
				if (0 == strcmp(m, MVAR_HOST_HOST) || 0 == strcmp(m, MVAR_HOSTNAME))
				{
					ret = DBget_trigger_value(event->trigger.expression, &replace_to, N_functionid,
							ZBX_REQUEST_HOST_HOST);
				}
				else if (0 == strcmp(m, MVAR_HOST_NAME))
				{
					ret = DBget_trigger_value(event->trigger.expression, &replace_to, N_functionid,
							ZBX_REQUEST_HOST_NAME);
				}
				else if (0 == strcmp(m, MVAR_HOST_IP) || 0 == strcmp(m, MVAR_IPADDRESS))
				{
					ret = DBget_trigger_value(event->trigger.expression, &replace_to, N_functionid,
							ZBX_REQUEST_HOST_IP);
				}
				else if (0 == strcmp(m, MVAR_HOST_DNS))
				{
					ret = DBget_trigger_value(event->trigger.expression, &replace_to, N_functionid,
							ZBX_REQUEST_HOST_DNS);
				}
				else if (0 == strcmp(m, MVAR_HOST_CONN))
				{
					ret = DBget_trigger_value(event->trigger.expression, &replace_to, N_functionid,
							ZBX_REQUEST_HOST_CONN);
				}
				else if (0 == strcmp(m, MVAR_HOST_PORT))
				{
					ret = DBget_trigger_value(event->trigger.expression, &replace_to, N_functionid,
							ZBX_REQUEST_HOST_PORT);
				}
				else if (0 == strncmp(m, MVAR_ITEM_LASTVALUE, ZBX_CONST_STRLEN(MVAR_ITEM_LASTVALUE)))
				{
					func_macro = 1;
					ret = DBitem_lastvalue(event->trigger.expression, &replace_to, N_functionid,
							raw_value);
				}
				else if (0 == strncmp(m, MVAR_ITEM_VALUE, ZBX_CONST_STRLEN(MVAR_ITEM_VALUE)))
				{
					func_macro = 1;
					ret = DBitem_value(event->trigger.expression, &replace_to, N_functionid,
							event->clock, event->ns, raw_value);
				}
				else if (0 == strncmp(m, "{$", 2))	/* user defined macros */
				{
					cache_trigger_hostids(&hostids, event->trigger.expression,
							event->trigger.recovery_expression);
					DCget_user_macro(hostids.values, hostids.values_num, m, &replace_to);
					pos = token.token.r;
				}
			}
		}
		else if (0 != (macro_type & MACRO_TYPE_TRIGGER_EXPRESSION))
		{
			if (EVENT_OBJECT_TRIGGER == event->object)
			{
				if (0 == strcmp(m, MVAR_TRIGGER_VALUE))
					replace_to = zbx_dsprintf(replace_to, "%d", event->value);

				/* when processing trigger expressions the user macros are already expanded */
			}
		}
		else if (0 != (macro_type & MACRO_TYPE_TRIGGER_URL))
		{
			if (EVENT_OBJECT_TRIGGER == event->object)
			{
				if (0 == strncmp(m, "{$", 2))	/* user defined macros */
				{
					cache_trigger_hostids(&hostids, event->trigger.expression,
							event->trigger.recovery_expression);
					DCget_user_macro(hostids.values, hostids.values_num, m, &replace_to);
					pos = token.token.r;
				}
				else if (0 == strcmp(m, MVAR_HOST_ID))
				{
					ret = DBget_trigger_value(event->trigger.expression, &replace_to, N_functionid,
							ZBX_REQUEST_HOST_ID);
				}
				else if (0 == strcmp(m, MVAR_HOST_HOST))
				{
					ret = DBget_trigger_value(event->trigger.expression, &replace_to, N_functionid,
							ZBX_REQUEST_HOST_HOST);
				}
				else if (0 == strcmp(m, MVAR_HOST_NAME))
				{
					ret = DBget_trigger_value(event->trigger.expression, &replace_to, N_functionid,
							ZBX_REQUEST_HOST_NAME);
				}
				else if (0 == strcmp(m, MVAR_HOST_IP))
				{
					ret = DBget_trigger_value(event->trigger.expression, &replace_to, N_functionid,
							ZBX_REQUEST_HOST_IP);
				}
				else if (0 == strcmp(m, MVAR_HOST_DNS))
				{
					ret = DBget_trigger_value(event->trigger.expression, &replace_to, N_functionid,
							ZBX_REQUEST_HOST_DNS);
				}
				else if (0 == strcmp(m, MVAR_HOST_CONN))
				{
					ret = DBget_trigger_value(event->trigger.expression, &replace_to, N_functionid,
							ZBX_REQUEST_HOST_CONN);
				}
				else if (0 == strcmp(m, MVAR_HOST_PORT))
				{
					ret = DBget_trigger_value(event->trigger.expression, &replace_to, N_functionid,
							ZBX_REQUEST_HOST_PORT);
				}
				else if (0 == strcmp(m, MVAR_TRIGGER_ID))
				{
					replace_to = zbx_dsprintf(replace_to, ZBX_FS_UI64, event->objectid);
				}
			}
		}
		else if (0 == indexed_macro &&
				0 != (macro_type & (MACRO_TYPE_ITEM_KEY | MACRO_TYPE_PARAMS_FIELD | MACRO_TYPE_LLD_FILTER)))
		{
			if (0 == strncmp(m, "{$", 2))	/* user defined macros */
			{
				DCget_user_macro(&dc_item->host.hostid, 1, m, &replace_to);
				pos = token.token.r;
			}
			else if (0 == strcmp(m, MVAR_HOST_HOST) || 0 == strcmp(m, MVAR_HOSTNAME))
				replace_to = zbx_strdup(replace_to, dc_item->host.host);
			else if (0 == strcmp(m, MVAR_HOST_NAME))
				replace_to = zbx_strdup(replace_to, dc_item->host.name);
			else if (0 == strcmp(m, MVAR_HOST_IP) || 0 == strcmp(m, MVAR_IPADDRESS))
			{
				if (INTERFACE_TYPE_UNKNOWN != dc_item->interface.type)
				{
					replace_to = zbx_strdup(replace_to, dc_item->interface.ip_orig);
				}
				else
				{
					ret = get_interface_value(dc_item->host.hostid, dc_item->itemid, &replace_to,
							ZBX_REQUEST_HOST_IP);
				}
			}
			else if	(0 == strcmp(m, MVAR_HOST_DNS))
			{
				if (INTERFACE_TYPE_UNKNOWN != dc_item->interface.type)
				{
					replace_to = zbx_strdup(replace_to, dc_item->interface.dns_orig);
				}
				else
				{
					ret = get_interface_value(dc_item->host.hostid, dc_item->itemid, &replace_to,
							ZBX_REQUEST_HOST_DNS);
				}
			}
			else if (0 == strcmp(m, MVAR_HOST_CONN))
			{
				if (INTERFACE_TYPE_UNKNOWN != dc_item->interface.type)
				{
					replace_to = zbx_strdup(replace_to, dc_item->interface.addr);
				}
				else
				{
					ret = get_interface_value(dc_item->host.hostid, dc_item->itemid, &replace_to,
							ZBX_REQUEST_HOST_CONN);
				}
			}
		}
		else if (0 == indexed_macro && 0 != (macro_type & MACRO_TYPE_INTERFACE_ADDR))
		{
			if (0 == strncmp(m, "{$", 2))	/* user defined macros */
			{
				DCget_user_macro(&dc_host->hostid, 1, m, &replace_to);
				pos = token.token.r;
			}
			else if (0 == strcmp(m, MVAR_HOST_HOST) || 0 == strcmp(m, MVAR_HOSTNAME))
				replace_to = zbx_strdup(replace_to, dc_host->host);
			else if (0 == strcmp(m, MVAR_HOST_NAME))
				replace_to = zbx_strdup(replace_to, dc_host->name);
			else if (0 == strcmp(m, MVAR_HOST_IP) || 0 == strcmp(m, MVAR_IPADDRESS))
			{
				if (SUCCEED == (ret = DCconfig_get_interface_by_type(&interface,
						dc_host->hostid, INTERFACE_TYPE_AGENT)))
				{
					replace_to = zbx_strdup(replace_to, interface.ip_orig);
				}
			}
			else if	(0 == strcmp(m, MVAR_HOST_DNS))
			{
				if (SUCCEED == (ret = DCconfig_get_interface_by_type(&interface,
						dc_host->hostid, INTERFACE_TYPE_AGENT)))
				{
					replace_to = zbx_strdup(replace_to, interface.dns_orig);
				}
			}
			else if (0 == strcmp(m, MVAR_HOST_CONN))
			{
				if (SUCCEED == (ret = DCconfig_get_interface_by_type(&interface,
						dc_host->hostid, INTERFACE_TYPE_AGENT)))
				{
					replace_to = zbx_strdup(replace_to, interface.addr);
				}
			}
		}
		else if (0 != (macro_type & (MACRO_TYPE_COMMON | MACRO_TYPE_SNMP_OID)))
		{
			if (0 == strncmp(m, "{$", 2))	/* user defined macros */
			{
				if (NULL != hostid)
					DCget_user_macro(hostid, 1, m, &replace_to);
				else
					DCget_user_macro(NULL, 0, m, &replace_to);

				pos = token.token.r;
			}
		}
		else if (0 != (macro_type & MACRO_TYPE_ITEM_EXPRESSION))
		{
			if (0 == strncmp(m, "{$", 2))	/* user defined macros */
			{
				require_numeric = 1;
				DCget_user_macro(&dc_host->hostid, 1, m, &replace_to);
				pos = token.token.r;
			}
		}
		else if (0 == indexed_macro && 0 != (macro_type & MACRO_TYPE_SCRIPT))
		{
			if (0 == strncmp(m, "{$", 2))	/* user defined macros */
			{
				DCget_user_macro(&dc_host->hostid, 1, m, &replace_to);
				pos = token.token.r;
			}
			else if (0 == strcmp(m, MVAR_HOST_HOST) || 0 == strcmp(m, MVAR_HOSTNAME))
				replace_to = zbx_strdup(replace_to, dc_host->host);
			else if (0 == strcmp(m, MVAR_HOST_NAME))
				replace_to = zbx_strdup(replace_to, dc_host->name);
			else if (0 == strcmp(m, MVAR_HOST_IP) || 0 == strcmp(m, MVAR_IPADDRESS))
			{
				if (SUCCEED == (ret = DCconfig_get_interface(&interface, dc_host->hostid, 0)))
					replace_to = zbx_strdup(replace_to, interface.ip_orig);
			}
			else if	(0 == strcmp(m, MVAR_HOST_DNS))
			{
				if (SUCCEED == (ret = DCconfig_get_interface(&interface, dc_host->hostid, 0)))
					replace_to = zbx_strdup(replace_to, interface.dns_orig);
			}
			else if (0 == strcmp(m, MVAR_HOST_CONN))
			{
				if (SUCCEED == (ret = DCconfig_get_interface(&interface, dc_host->hostid, 0)))
					replace_to = zbx_strdup(replace_to, interface.addr);
			}
		}
		else if (0 == indexed_macro && 0 != (macro_type & MACRO_TYPE_HTTPTEST_FIELD))
		{
			if (0 == strncmp(m, "{$", 2))	/* user defined macros */
			{
				DCget_user_macro(&dc_host->hostid, 1, m, &replace_to);
				pos = token.token.r;
			}
			else if (0 == strcmp(m, MVAR_HOST_HOST) || 0 == strcmp(m, MVAR_HOSTNAME))
				replace_to = zbx_strdup(replace_to, dc_host->host);
			else if (0 == strcmp(m, MVAR_HOST_NAME))
				replace_to = zbx_strdup(replace_to, dc_host->name);
			else if (0 == strcmp(m, MVAR_HOST_IP) || 0 == strcmp(m, MVAR_IPADDRESS))
			{
				if (SUCCEED == (ret = DCconfig_get_interface(&interface, dc_host->hostid, 0)))
					replace_to = zbx_strdup(replace_to, interface.ip_orig);
			}
			else if	(0 == strcmp(m, MVAR_HOST_DNS))
			{
				if (SUCCEED == (ret = DCconfig_get_interface(&interface, dc_host->hostid, 0)))
					replace_to = zbx_strdup(replace_to, interface.dns_orig);
			}
			else if (0 == strcmp(m, MVAR_HOST_CONN))
			{
				if (SUCCEED == (ret = DCconfig_get_interface(&interface, dc_host->hostid, 0)))
					replace_to = zbx_strdup(replace_to, interface.addr);
			}
		}
		else if (0 == indexed_macro && 0 != (macro_type & MACRO_TYPE_ALERT))
		{
			if (0 == strcmp(m, MVAR_ALERT_SENDTO))
				replace_to = zbx_strdup(replace_to, alert->sendto);
			else if (0 == strcmp(m, MVAR_ALERT_SUBJECT))
				replace_to = zbx_strdup(replace_to, alert->subject);
			else if (0 == strcmp(m, MVAR_ALERT_MESSAGE))
				replace_to = zbx_strdup(replace_to, alert->message);
		}
		else if (macro_type & MACRO_TYPE_TRIGGER_TAG)
		{
			if (EVENT_SOURCE_TRIGGERS == event->source)
			{
				if (0 == strncmp(m, MVAR_ITEM_LASTVALUE, ZBX_CONST_STRLEN(MVAR_ITEM_LASTVALUE)))
				{
					func_macro = 1;
					ret = DBitem_lastvalue(event->trigger.expression, &replace_to, N_functionid,
							raw_value);
				}
				else if (0 == strncmp(m, MVAR_ITEM_VALUE, ZBX_CONST_STRLEN(MVAR_ITEM_VALUE)))
				{
					func_macro = 1;
					ret = DBitem_value(event->trigger.expression, &replace_to, N_functionid,
							event->clock, event->ns, raw_value);
				}
				else if (0 == strncmp(m, "{$", 2))	/* user defined macros */
				{
					cache_trigger_hostids(&hostids, event->trigger.expression,
							event->trigger.recovery_expression);
					DCget_user_macro(hostids.values, hostids.values_num, m, &replace_to);
					pos = token.token.r;
				}
			}
		}

		if (ZBX_TOKEN_FUNC_MACRO == token.type && NULL != replace_to)
		{
			if (0 != func_macro)
			{
				if (SUCCEED != (ret = zbx_calculate_macro_function(*data + token.data.func_macro.func.l,
						&replace_to)))
				{
					zbx_free(replace_to);
				}
			}
			else
			{
				/* ignore functions with macros not supporting them */
				zbx_free(replace_to);
			}
		}

		if (1 == require_numeric && NULL != replace_to)
		{
			if (SUCCEED == (res = is_double_suffix(replace_to, ZBX_FLAG_DOUBLE_SUFFIX)))
				wrap_negative_double_suffix(&replace_to, NULL);
			else if (NULL != error)
				zbx_snprintf(error, maxerrlen, "Macro '%s' value is not numeric", m);
		}

		if (FAIL == ret)
		{
			zabbix_log(LOG_LEVEL_DEBUG, "cannot resolve macro '%s'", bl);
			replace_to = zbx_strdup(replace_to, STR_UNKNOWN_VARIABLE);
		}

		*br = c;

		if (NULL != replace_to)
		{
			size_t	sz_m, sz_r;

			pos = token.token.r;

			sz_m = br - bl;
			sz_r = strlen(replace_to);

			if (sz_m != sz_r)
			{
				pos += sz_r - sz_m;
				data_len += sz_r - sz_m;

				if (data_len > data_alloc)
				{
					char	*old_data = *data;

					while (data_len > data_alloc)
						data_alloc *= 2;
					*data = zbx_realloc(*data, data_alloc);
					bl += *data - old_data;
				}

				memmove(bl + sz_r, bl + sz_m, data_len - (bl - *data) - sz_r);
			}

			memcpy(bl, replace_to, sz_r);
			zbx_free(replace_to);
		}

		pos++;
	}

	zbx_vector_uint64_destroy(&hostids);

	zabbix_log(LOG_LEVEL_DEBUG, "End %s() data:'%s'", __function_name, *data);

	return res;
}

static int	extract_expression_functionids(zbx_vector_uint64_t *functionids, const char *expression)
{
	const char	*bl, *br;
	zbx_uint64_t	functionid;

	for (bl = strchr(expression, '{'); NULL != bl; bl = strchr(bl, '{'))
	{
		if (NULL == (br = strchr(bl, '}')))
			break;

		if (SUCCEED != is_uint64_n(bl + 1, br - bl - 1, &functionid))
			break;

		zbx_vector_uint64_append(functionids, functionid);

		bl = br + 1;
	}

	return (NULL == bl ? SUCCEED : FAIL);
}

static void	zbx_extract_functionids(zbx_vector_uint64_t *functionids, zbx_vector_ptr_t *triggers)
{
	const char	*__function_name = "zbx_extract_functionids";

	DC_TRIGGER	*tr;
	int		i, values_num_save;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() tr_num:%d", __function_name, triggers->values_num);

	for (i = 0; i < triggers->values_num; i++)
	{
		const char	*error_expression = NULL;

		tr = (DC_TRIGGER *)triggers->values[i];

		if (NULL != tr->new_error)
			continue;

		values_num_save = functionids->values_num;

		if (SUCCEED != extract_expression_functionids(functionids, tr->expression))
		{
			error_expression = tr->expression;
		}
		else if (TRIGGER_RECOVERY_MODE_RECOVERY_EXPRESSION == tr->recovery_mode &&
				SUCCEED != extract_expression_functionids(functionids, tr->recovery_expression))
		{
			error_expression = tr->recovery_expression;
		}

		if (NULL != error_expression)
		{
			tr->new_error = zbx_dsprintf(tr->new_error, "Invalid expression [%s]", error_expression);
			tr->new_value = TRIGGER_VALUE_UNKNOWN;
			functionids->values_num = values_num_save;
		}
	}

	zbx_vector_uint64_sort(functionids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
	zbx_vector_uint64_uniq(functionids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s() functionids_num:%d", __function_name, functionids->values_num);
}

typedef struct
{
	DC_TRIGGER	*trigger;
	int		start_index;
	int		count;
}
zbx_trigger_func_position_t;

/******************************************************************************
 *                                                                            *
 * Function: expand_trigger_macros                                            *
 *                                                                            *
 * Purpose: expand macros in a trigger expression                             *
 *                                                                            *
 * Parameters: event - The trigger event structure                            *
 *             trigger - The trigger where to expand macros in                *
 *                                                                            *
 * Author: Andrea Biscuola                                                    *
 *                                                                            *
 ******************************************************************************/
static void	expand_trigger_macros(DB_EVENT *event, DC_TRIGGER *trigger)
{
	substitute_simple_macros(NULL, event, NULL, NULL, NULL, NULL, NULL, NULL, &trigger->expression,
			MACRO_TYPE_TRIGGER_EXPRESSION, NULL, 0);

	if (TRIGGER_RECOVERY_MODE_RECOVERY_EXPRESSION == trigger->recovery_mode)
	{
		substitute_simple_macros(NULL, event, NULL, NULL, NULL, NULL, NULL, NULL,
				&trigger->recovery_expression, MACRO_TYPE_TRIGGER_EXPRESSION, NULL, 0);
	}
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_link_triggers_with_functions                                 *
 *                                                                            *
 * Purpose: triggers links with functions                                     *
 *                                                                            *
 * Parameters: triggers_func_pos - [IN/OUT] pointer to the list of triggers   *
 *                                 with functions position in functionids     *
 *                                 array                                      *
 *             functionids       - [IN/OUT] array of function IDs             *
 *             trigger_order     - [IN] array of triggers                     *
 *                                                                            *
 ******************************************************************************/
static void	zbx_link_triggers_with_functions(zbx_vector_ptr_t *triggers_func_pos, zbx_vector_uint64_t *functionids,
		zbx_vector_ptr_t *trigger_order)
{
	const char		*__function_name = "zbx_link_triggers_with_functions";

	zbx_vector_uint64_t	funcids;
	DC_TRIGGER		*tr;
	DB_EVENT		ev;
	int			i;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() trigger_order_num:%d", __function_name, trigger_order->values_num);

	zbx_vector_uint64_create(&funcids);
	zbx_vector_uint64_reserve(&funcids, functionids->values_num);

	ev.object = EVENT_OBJECT_TRIGGER;

	for (i = 0; i < trigger_order->values_num; i++)
	{
		zbx_trigger_func_position_t	*tr_func_pos;

		tr = (DC_TRIGGER *)trigger_order->values[i];

		if (NULL != tr->new_error)
			continue;

		ev.value = tr->value;

		expand_trigger_macros(&ev, tr);

		if (SUCCEED == extract_expression_functionids(&funcids, tr->expression))
		{
			tr_func_pos = zbx_malloc(NULL, sizeof(zbx_trigger_func_position_t));
			tr_func_pos->trigger = tr;
			tr_func_pos->start_index = functionids->values_num;
			tr_func_pos->count = funcids.values_num;

			zbx_vector_uint64_append_array(functionids, funcids.values, funcids.values_num);
			zbx_vector_ptr_append(triggers_func_pos, tr_func_pos);
		}

		zbx_vector_uint64_clear(&funcids);
	}

	zbx_vector_uint64_destroy(&funcids);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s() triggers_func_pos_num:%d", __function_name,
			triggers_func_pos->values_num);
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_determine_items_in_expressions                               *
 *                                                                            *
 * Purpose: mark triggers that use one of the items in problem expression     *
 *          with ZBX_DC_TRIGGER_PROBLEM_EXPRESSION flag                       *
 *                                                                            *
 * Parameters: trigger_order - [IN/OUT] pointer to the list of triggers       *
 *             itemids       - [IN] array of item IDs                         *
 *             item_num      - [IN] number of items                           *
 *                                                                            *
 ******************************************************************************/
void	zbx_determine_items_in_expressions(zbx_vector_ptr_t *trigger_order, const zbx_uint64_t *itemids, int item_num)
{
	zbx_vector_ptr_t	triggers_func_pos;
	zbx_vector_uint64_t	functionids, itemids_sorted;
	DC_FUNCTION		*functions = NULL;
	int			*errcodes = NULL, t, f;

	zbx_vector_uint64_create(&itemids_sorted);
	zbx_vector_uint64_append_array(&itemids_sorted, itemids, item_num);
	zbx_vector_uint64_sort(&itemids_sorted, ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	zbx_vector_ptr_create(&triggers_func_pos);
	zbx_vector_ptr_reserve(&triggers_func_pos, trigger_order->values_num);

	zbx_vector_uint64_create(&functionids);
	zbx_vector_uint64_reserve(&functionids, item_num);

	zbx_link_triggers_with_functions(&triggers_func_pos, &functionids, trigger_order);

	functions = zbx_malloc(functions, sizeof(DC_FUNCTION) * functionids.values_num);
	errcodes = zbx_malloc(errcodes, sizeof(int) * functionids.values_num);

	DCconfig_get_functions_by_functionids(functions, functionids.values, errcodes, functionids.values_num);

	for (t = 0; t < triggers_func_pos.values_num; t++)
	{
		zbx_trigger_func_position_t	*func_pos = (zbx_trigger_func_position_t *)triggers_func_pos.values[t];

		for (f = func_pos->start_index; f < func_pos->start_index + func_pos->count; f++)
		{
			if (FAIL != zbx_vector_uint64_bsearch(&itemids_sorted, functions[f].itemid,
					ZBX_DEFAULT_UINT64_COMPARE_FUNC))
			{
				func_pos->trigger->flags |= ZBX_DC_TRIGGER_PROBLEM_EXPRESSION;
				break;
			}
		}
	}

	DCconfig_clean_functions(functions, errcodes, functionids.values_num);
	zbx_free(errcodes);
	zbx_free(functions);

	zbx_vector_ptr_clear_ext(&triggers_func_pos, zbx_ptr_free);
	zbx_vector_ptr_destroy(&triggers_func_pos);

	zbx_vector_uint64_clear(&functionids);
	zbx_vector_uint64_destroy(&functionids);

	zbx_vector_uint64_clear(&itemids_sorted);
	zbx_vector_uint64_destroy(&itemids_sorted);
}

typedef struct
{
	/* input data */
	zbx_uint64_t	itemid;
	char		*function;
	char		*parameter;
	zbx_timespec_t	timespec;

	/* output data */
	char		*value;
	char		*error;
}
zbx_func_t;

typedef struct
{
	zbx_uint64_t	functionid;
	zbx_func_t	*func;
}
zbx_ifunc_t;

static zbx_hash_t	func_hash_func(const void *data)
{
	const zbx_func_t	*func = (const zbx_func_t *)data;
	zbx_hash_t		hash;

	hash = ZBX_DEFAULT_UINT64_HASH_FUNC(&func->itemid);
	hash = ZBX_DEFAULT_STRING_HASH_ALGO(func->function, strlen(func->function), hash);
	hash = ZBX_DEFAULT_STRING_HASH_ALGO(func->parameter, strlen(func->parameter), hash);
	hash = ZBX_DEFAULT_HASH_ALGO(&func->timespec.sec, sizeof(func->timespec.sec), hash);
	hash = ZBX_DEFAULT_HASH_ALGO(&func->timespec.ns, sizeof(func->timespec.ns), hash);

	return hash;
}

static int	func_compare_func(const void *d1, const void *d2)
{
	const zbx_func_t	*func1 = (const zbx_func_t *)d1;
	const zbx_func_t	*func2 = (const zbx_func_t *)d2;
	int			ret;

	ZBX_RETURN_IF_NOT_EQUAL(func1->itemid, func2->itemid);

	if (0 != (ret = strcmp(func1->function, func2->function)))
		return ret;

	if (0 != (ret = strcmp(func1->parameter, func2->parameter)))
		return ret;

	ZBX_RETURN_IF_NOT_EQUAL(func1->timespec.sec, func2->timespec.sec);
	ZBX_RETURN_IF_NOT_EQUAL(func1->timespec.ns, func2->timespec.ns);

	return 0;
}

static void	func_clean(void *ptr)
{
	zbx_func_t	*func = (zbx_func_t *)ptr;

	zbx_free(func->function);
	zbx_free(func->parameter);
	zbx_free(func->value);
	zbx_free(func->error);
}

static void	zbx_populate_function_items(zbx_vector_uint64_t *functionids, zbx_hashset_t *funcs,
		zbx_hashset_t *ifuncs, zbx_vector_ptr_t *triggers)
{
	const char	*__function_name = "zbx_populate_function_items";

	int		i, j;
	DC_TRIGGER	*tr;
	DC_FUNCTION	*functions = NULL;
	int		*errcodes = NULL;
	zbx_ifunc_t	ifunc_local;
	zbx_func_t	*func, func_local;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() functionids_num:%d", __function_name, functionids->values_num);

	func_local.value = NULL;
	func_local.error = NULL;

	functions = zbx_malloc(functions, sizeof(DC_FUNCTION) * functionids->values_num);
	errcodes = zbx_malloc(errcodes, sizeof(int) * functionids->values_num);

	DCconfig_get_functions_by_functionids(functions, functionids->values, errcodes, functionids->values_num);

	for (i = 0; i < functionids->values_num; i++)
	{
		if (SUCCEED != errcodes[i])
			continue;

		func_local.itemid = functions[i].itemid;

		if (FAIL != (j = zbx_vector_ptr_bsearch(triggers, &functions[i].triggerid,
				ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC)))
		{
			tr = (DC_TRIGGER *)triggers->values[j];
			func_local.timespec = tr->timespec;
		}
		else
		{
			func_local.timespec.sec = 0;
			func_local.timespec.ns = 0;
		}

		func_local.function = functions[i].function;
		func_local.parameter = functions[i].parameter;

		if (NULL == (func = zbx_hashset_search(funcs, &func_local)))
		{
			func = zbx_hashset_insert(funcs, &func_local, sizeof(func_local));
			func->function = zbx_strdup(NULL, func_local.function);
			func->parameter = zbx_strdup(NULL, func_local.parameter);
		}

		ifunc_local.functionid = functions[i].functionid;
		ifunc_local.func = func;
		zbx_hashset_insert(ifuncs, &ifunc_local, sizeof(ifunc_local));
	}

	DCconfig_clean_functions(functions, errcodes, functionids->values_num);

	zbx_free(errcodes);
	zbx_free(functions);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s() ifuncs_num:%d", __function_name, ifuncs->num_data);
}

static void	zbx_evaluate_item_functions(zbx_hashset_t *funcs, zbx_vector_ptr_t *unknown_msgs)
{
	const char	*__function_name = "zbx_evaluate_item_functions";

	DC_ITEM			*items = NULL;
	char			value[MAX_BUFFER_LEN], *error = NULL;
	int			i;
	zbx_func_t		*func;
	zbx_vector_uint64_t	itemids;
	int			*errcodes = NULL;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() funcs_num:%d", __function_name, funcs->num_data);

	zbx_vector_uint64_create(&itemids);
	zbx_vector_uint64_reserve(&itemids, funcs->num_data);

	zbx_hashset_iter_reset(funcs, &iter);
	while (NULL != (func = zbx_hashset_iter_next(&iter)))
		zbx_vector_uint64_append(&itemids, func->itemid);

	zbx_vector_uint64_sort(&itemids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
	zbx_vector_uint64_uniq(&itemids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	items = zbx_malloc(items, sizeof(DC_ITEM) * (size_t)itemids.values_num);
	errcodes = zbx_malloc(errcodes, sizeof(int) * (size_t)itemids.values_num);

	DCconfig_get_items_by_itemids(items, itemids.values, errcodes, itemids.values_num);

	zbx_hashset_iter_reset(funcs, &iter);
	while (NULL != (func = zbx_hashset_iter_next(&iter)))
	{
		int	ret_unknown = 0;	/* flag raised if current function evaluates to ZBX_UNKNOWN */
		char	*unknown_msg;

		i = zbx_vector_uint64_bsearch(&itemids, func->itemid, ZBX_DEFAULT_UINT64_COMPARE_FUNC);

		if (SUCCEED != errcodes[i])
		{
			func->error = zbx_dsprintf(func->error, "Cannot evaluate function \"%s(%s)\":"
					" item does not exist.",
					func->function, func->parameter);
			continue;
		}

		/* do not evaluate if the item is disabled or belongs to a disabled host */

		if (ITEM_STATUS_ACTIVE != items[i].status)
		{
			func->error = zbx_dsprintf(func->error, "Cannot evaluate function \"%s:%s.%s(%s)\":"
					" item is disabled.",
					items[i].host.host, items[i].key_orig, func->function, func->parameter);
			continue;
		}

		if (HOST_STATUS_MONITORED != items[i].host.status)
		{
			func->error = zbx_dsprintf(func->error, "Cannot evaluate function \"%s:%s.%s(%s)\":"
					" item belongs to a disabled host.",
					items[i].host.host, items[i].key_orig, func->function, func->parameter);
			continue;
		}

		/* If the item is NOTSUPPORTED then evaluation is allowed for:   */
		/*   - time-based functions and nodata(). Their values can be    */
		/*     evaluated to regular numbers even for NOTSUPPORTED items. */
		/*   - other functions. Result of evaluation is ZBX_UNKNOWN.     */

		if (ITEM_STATE_NOTSUPPORTED == items[i].state &&
				FAIL == evaluatable_for_notsupported(func->function))
		{
			/* compose and store 'unknown' message for future use */
			unknown_msg = zbx_dsprintf(NULL,
					"Cannot evaluate function \"%s:%s.%s(%s)\": item is not supported.",
					items[i].host.host, items[i].key_orig, func->function, func->parameter);

			zbx_free(func->error);
			zbx_vector_ptr_append(unknown_msgs, unknown_msg);
			ret_unknown = 1;
		}

		if (0 == ret_unknown && SUCCEED != evaluate_function(value, &items[i], func->function,
				func->parameter, func->timespec.sec, &error))
		{
			/* compose and store error message for future use */
			if (NULL != error)
			{
				unknown_msg = zbx_dsprintf(NULL,
						"Cannot evaluate function \"%s:%s.%s(%s)\": %s.",
						items[i].host.host, items[i].key_orig, func->function,
						func->parameter, error);

				zbx_free(func->error);
				zbx_free(error);
			}
			else
			{
				unknown_msg = zbx_dsprintf(NULL,
						"Cannot evaluate function \"%s:%s.%s(%s)\".",
						items[i].host.host, items[i].key_orig,
						func->function, func->parameter);

				zbx_free(func->error);
			}

			zbx_vector_ptr_append(unknown_msgs, unknown_msg);
			ret_unknown = 1;
		}

		if (0 == ret_unknown)
		{
			func->value = zbx_strdup(func->value, value);
		}
		else
		{
			/* write a special token of unknown value with 'unknown' message number, like */
			/* ZBX_UNKNOWN0, ZBX_UNKNOWN1 etc. not wrapped in () */
			func->value = zbx_dsprintf(func->value, ZBX_UNKNOWN_STR "%d",
					unknown_msgs->values_num - 1);
		}
	}

	DCconfig_clean_items(items, errcodes, itemids.values_num);
	zbx_vector_uint64_destroy(&itemids);

	zbx_free(errcodes);
	zbx_free(items);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

static int	substitute_expression_functions_results(zbx_hashset_t *ifuncs, char *expression, char **out,
		size_t *out_alloc, char **error)
{
	char			*br, *bl;
	size_t			out_offset = 0;
	zbx_uint64_t		functionid;
	zbx_func_t		*func;
	zbx_ifunc_t		*ifunc;

	for (br = expression, bl = strchr(expression, '{'); NULL != bl; bl = strchr(bl, '{'))
	{
		*bl = '\0';
		zbx_strcpy_alloc(out, out_alloc, &out_offset, br);
		*bl = '{';

		if (NULL == (br = strchr(bl, '}')))
		{
			*error = zbx_strdup(*error, "Invalid trigger expression");
			return FAIL;
		}

		*br = '\0';

		ZBX_STR2UINT64(functionid, bl + 1);

		*br++ = '}';
		bl = br;

		if (NULL == (ifunc = zbx_hashset_search(ifuncs, &functionid)))
		{
			*error = zbx_dsprintf(*error, "Cannot obtain function"
					" and item for functionid: " ZBX_FS_UI64, functionid);
			return FAIL;
		}

		func = ifunc->func;

		if (NULL != func->error)
		{
			*error = zbx_strdup(*error, func->error);
			return FAIL;
		}

		if (NULL == func->value)
		{
			*error = zbx_strdup(*error, "Unexpected error while processing a trigger expression");
			return FAIL;
		}

		if (SUCCEED != is_double_suffix(func->value, ZBX_FLAG_DOUBLE_SUFFIX) || '-' == *func->value)
		{
			zbx_chrcpy_alloc(out, out_alloc, &out_offset, '(');
			zbx_strcpy_alloc(out, out_alloc, &out_offset, func->value);
			zbx_chrcpy_alloc(out, out_alloc, &out_offset, ')');
		}
		else
			zbx_strcpy_alloc(out, out_alloc, &out_offset, func->value);
	}

	zbx_strcpy_alloc(out, out_alloc, &out_offset, br);

	return SUCCEED;
}

static void	zbx_substitute_functions_results(zbx_hashset_t *ifuncs, zbx_vector_ptr_t *triggers)
{
	const char		*__function_name = "zbx_substitute_functions_results";

	DC_TRIGGER		*tr;
	char			*out = NULL;
	size_t			out_alloc = TRIGGER_EXPRESSION_LEN_MAX;
	int			i;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() ifuncs_num:%d tr_num:%d",
			__function_name, ifuncs->num_data, triggers->values_num);

	out = zbx_malloc(out, out_alloc);

	for (i = 0; i < triggers->values_num; i++)
	{
		tr = (DC_TRIGGER *)triggers->values[i];

		if (NULL != tr->new_error)
			continue;

		if( SUCCEED != substitute_expression_functions_results(ifuncs, tr->expression, &out, &out_alloc,
				&tr->new_error))
		{
			tr->new_value = TRIGGER_VALUE_UNKNOWN;
			continue;
		}

		zabbix_log(LOG_LEVEL_DEBUG, "%s() expression[%d]:'%s' => '%s'", __function_name, i,
				tr->expression, out);

		tr->expression = zbx_strdup(tr->expression, out);

		if (TRIGGER_RECOVERY_MODE_RECOVERY_EXPRESSION == tr->recovery_mode)
		{
			if (SUCCEED != substitute_expression_functions_results(ifuncs,
					tr->recovery_expression, &out, &out_alloc, &tr->new_error))
			{
				tr->new_value = TRIGGER_VALUE_UNKNOWN;
				continue;
			}

			zabbix_log(LOG_LEVEL_DEBUG, "%s() recovery_expression[%d]:'%s' => '%s'", __function_name, i,
					tr->recovery_expression, out);

			tr->recovery_expression = zbx_strdup(tr->recovery_expression, out);
		}
	}

	zbx_free(out);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: substitute_functions                                             *
 *                                                                            *
 * Purpose: substitute expression functions with their values                 *
 *                                                                            *
 * Parameters: triggers - array of DC_TRIGGER structures                      *
 *             unknown_msgs - vector for storing messages for NOTSUPPORTED    *
 *                            items and failed functions                      *
 *                                                                            *
 * Author: Alexei Vladishev, Alexander Vladishev, Aleksandrs Saveljevs        *
 *                                                                            *
 * Comments: example: "({15}>10) or ({123}=1)" => "(26.416>10) or (0=1)"      *
 *                                                                            *
 ******************************************************************************/
static void	substitute_functions(zbx_vector_ptr_t *triggers, zbx_vector_ptr_t *unknown_msgs)
{
	const char		*__function_name = "substitute_functions";

	zbx_vector_uint64_t	functionids;
	zbx_hashset_t		ifuncs, funcs;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_vector_uint64_create(&functionids);
	zbx_extract_functionids(&functionids, triggers);

	if (0 == functionids.values_num)
		goto empty;

	zbx_hashset_create(&ifuncs, triggers->values_num, ZBX_DEFAULT_UINT64_HASH_FUNC,
			ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	zbx_hashset_create_ext(&funcs, triggers->values_num, func_hash_func, func_compare_func, func_clean,
				ZBX_DEFAULT_MEM_MALLOC_FUNC, ZBX_DEFAULT_MEM_REALLOC_FUNC, ZBX_DEFAULT_MEM_FREE_FUNC);

	zbx_populate_function_items(&functionids, &funcs, &ifuncs, triggers);

	if (0 != ifuncs.num_data)
	{
		zbx_evaluate_item_functions(&funcs, unknown_msgs);
		zbx_substitute_functions_results(&ifuncs, triggers);
	}

	zbx_hashset_destroy(&ifuncs);
	zbx_hashset_destroy(&funcs);
empty:
	zbx_vector_uint64_destroy(&functionids);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: evaluate_expressions                                             *
 *                                                                            *
 * Purpose: evaluate trigger expressions                                      *
 *                                                                            *
 * Parameters: triggers - [IN] array of DC_TRIGGER structures                 *
 *                                                                            *
 * Author: Alexei Vladishev                                                   *
 *                                                                            *
 ******************************************************************************/
void	evaluate_expressions(zbx_vector_ptr_t *triggers)
{
	const char	*__function_name = "evaluate_expressions";

	DB_EVENT		event;
	DC_TRIGGER		*tr;
	int			i;
	double			expr_result;
	zbx_vector_ptr_t	unknown_msgs;	    /* pointers to messages about origins of 'unknown' values */
	char			err[MAX_STRING_LEN];

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() tr_num:%d", __function_name, triggers->values_num);

	event.object = EVENT_OBJECT_TRIGGER;

	for (i = 0; i < triggers->values_num; i++)
	{
		tr = (DC_TRIGGER *)triggers->values[i];

		event.value = tr->value;

		if (NULL == tr->new_error)
		{
			expand_trigger_macros(&event, tr);
		}
		else
		{
			zbx_snprintf(err, sizeof(err), "Cannot evaluate expression: %s.", tr->new_error);
			tr->new_error = zbx_strdup(tr->new_error, err);
			tr->new_value = TRIGGER_VALUE_UNKNOWN;
		}
	}

	/* Assumption: most often there will be no NOTSUPPORTED items and function errors. */
	/* Therefore initialize error messages vector but do not reserve any space. */
	zbx_vector_ptr_create(&unknown_msgs);

	substitute_functions(triggers, &unknown_msgs);

	/* calculate new trigger values based on their recovery modes and expression evaluations */
	for (i = 0; i < triggers->values_num; i++)
	{
		tr = (DC_TRIGGER *)triggers->values[i];

		if (NULL != tr->new_error)
			continue;

		if (SUCCEED != evaluate(&expr_result, tr->expression, err, sizeof(err), &unknown_msgs))
		{
			tr->new_error = zbx_strdup(tr->new_error, err);
			tr->new_value = TRIGGER_VALUE_UNKNOWN;
			continue;
		}

		/* trigger expression evaluates to true, set PROBLEM value */
		if (SUCCEED != zbx_double_compare(expr_result, 0.0))
		{
			if (0 == (tr->flags & ZBX_DC_TRIGGER_PROBLEM_EXPRESSION))
			{
				/* trigger value should remain unchanged and no PROBLEM events should be generated if */
				/* problem expression evaluates to true, but trigger recalculation was initiated by a */
				/* time-based function or a new value of an item in recovery expression */
				tr->new_value = TRIGGER_VALUE_NONE;
			}
			else
				tr->new_value = TRIGGER_VALUE_PROBLEM;

			continue;
		}

		/* otherwise try to recover trigger by setting OK value */
		if (TRIGGER_VALUE_PROBLEM == tr->value && TRIGGER_RECOVERY_MODE_NONE != tr->recovery_mode)
		{
			if (TRIGGER_RECOVERY_MODE_EXPRESSION == tr->recovery_mode)
			{
				tr->new_value = TRIGGER_VALUE_OK;
				continue;
			}

			/* processing recovery expression mode */
			if (SUCCEED != evaluate(&expr_result, tr->recovery_expression, err, sizeof(err), &unknown_msgs))
			{
				tr->new_error = zbx_strdup(tr->new_error, err);
				tr->new_value = TRIGGER_VALUE_UNKNOWN;
				continue;
			}

			if (SUCCEED != zbx_double_compare(expr_result, 0.0))
			{
				tr->new_value = TRIGGER_VALUE_OK;
				continue;
			}
		}

		/* no changes, keep the old value */
		tr->new_value = TRIGGER_VALUE_NONE;
	}

	zbx_vector_ptr_clear_ext(&unknown_msgs, zbx_ptr_free);
	zbx_vector_ptr_destroy(&unknown_msgs);

	if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
	{
		for (i = 0; i < triggers->values_num; i++)
		{
			tr = (DC_TRIGGER *)triggers->values[i];

			if (NULL != tr->new_error)
			{
				zabbix_log(LOG_LEVEL_DEBUG, "%s():expression [%s] cannot be evaluated: %s",
						__function_name, tr->expression, tr->new_error);
			}
		}

		zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
	}
}

/******************************************************************************
 *                                                                            *
 * Function: process_simple_macro_token                                       *
 *                                                                            *
 * Purpose: trying to resolve the discovery macros in item key parameters     *
 *          in simple macros like {host:key[].func()}                         *
 *                                                                            *
 * Comments: This function could be improved by using zbx_token_simple_macro_t*
 *           structure host, key, func fields.                                *
 *                                                                            *
 ******************************************************************************/
static int	process_simple_macro_token(char **data, zbx_token_t *token, const struct zbx_json_parse *jp_row)
{
	char	*pl, *pr, *key = NULL, *replace_to = NULL;
	size_t	sz, replace_to_offset = 0, replace_to_alloc = 0, par_l, par_r;
	int	ret = FAIL;

	pl = pr = *data + token->token.l;
	if ('{' != *pr++)
		return FAIL;

	/* check for macros {HOST.HOST<1-9>} and {HOSTNAME<1-9>} */
	if ((0 == strncmp(pr, MVAR_HOST_HOST, sz = sizeof(MVAR_HOST_HOST) - 2) ||
			0 == strncmp(pr, MVAR_HOSTNAME, sz = sizeof(MVAR_HOSTNAME) - 2)) &&
			('}' == pr[sz] || ('}' == pr[sz + 1] && '1' <= pr[sz] && pr[sz] <= '9')))
	{
		pr += sz + ('}' == pr[sz] ? 1 : 2);
	}
	else if (SUCCEED != parse_host(&pr, NULL))	/* a simple host name; e.g. "Zabbix server" */
		return FAIL;

	if (':' != *pr++)
		return FAIL;

	replace_to_alloc = 128;
	replace_to = zbx_malloc(NULL, replace_to_alloc);

	zbx_strncpy_alloc(&replace_to, &replace_to_alloc, &replace_to_offset, pl, pr - pl);

	/* an item key */
	if (SUCCEED != get_item_key(&pr, &key))
		goto clean;

	substitute_key_macros(&key, NULL, NULL, jp_row, MACRO_TYPE_ITEM_KEY, NULL, 0);
	zbx_strcpy_alloc(&replace_to, &replace_to_alloc, &replace_to_offset, key);

	zbx_free(key);

	pl = pr;

	if ('.' != *pr++)
		goto clean;

	/* a trigger function with parameters */
	if (SUCCEED != zbx_function_validate(pr, &par_l, &par_r))
		goto clean;

	pr += par_r + 1;

	if ('}' != *pr++)
		goto clean;

	zbx_strncpy_alloc(&replace_to, &replace_to_alloc, &replace_to_offset, pl, pr - pl);

	token->token.r = pr - *data - 1;
	zbx_replace_string(data, token->token.l, &token->token.r, replace_to);

	ret = SUCCEED;
clean:
	zbx_free(replace_to);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: process_lld_macro_token                                          *
 *                                                                            *
 * Purpose: expand discovery macro in expression                              *
 *                                                                            *
 * Parameters: data      - [IN/OUT] the expression containing lld macro       *
 *             token     - [IN/OUT] the token with lld macro location data    *
 *             flags     - [IN] the flags passed to                           *
 *                                  subtitute_discovery_macros() function     *
 *             jp_row    - [IN] discovery data                                *
 *             error     - [OUT] should be not NULL if                        *
 *                               ZBX_MACRO_NUMERIC flag is set                *
 *             error_len - [IN] the size of error buffer                      *
 *                                                                            *
 * Return value: Always SUCCEED if numeric flag is not set, otherwise SUCCEED *
 *               if all discovery macros resolved to numeric values,          *
 *               otherwise FAIL with an error message.                        *
 *                                                                            *
 ******************************************************************************/
static int	process_lld_macro_token(char **data, zbx_token_t *token, int flags,
		const struct zbx_json_parse *jp_row, char *error, size_t error_len)
{
	char	c, *replace_to = NULL;
	int	ret = SUCCEED;
	size_t	replace_to_alloc = 0;

	c = (*data)[token->token.r + 1];
	(*data)[token->token.r + 1] = '\0';

	if (SUCCEED != zbx_json_value_by_name_dyn(jp_row, *data + token->token.l, &replace_to, &replace_to_alloc))
	{
		zabbix_log(LOG_LEVEL_DEBUG, "cannot substitute macro \"%s\": not found in value set",
				*data + token->token.l);

		if (0 != (flags & ZBX_TOKEN_NUMERIC))
		{
			zbx_snprintf(error, error_len, "no value for macro \"%s\"", *data + token->token.l);
			ret = FAIL;
		}

		zbx_free(replace_to);
	}
	else if (0 != (flags & ZBX_TOKEN_NUMERIC))
	{
		if (SUCCEED == (ret = is_double_suffix(replace_to, ZBX_FLAG_DOUBLE_SUFFIX)))
		{
			wrap_negative_double_suffix(&replace_to, &replace_to_alloc);
		}
		else
		{
			zbx_free(replace_to);
			zbx_snprintf(error, error_len, "macro \"%s\" value is not numeric", *data + token->token.l);
			ret = FAIL;
		}
	}

	(*data)[token->token.r + 1] = c;

	if (NULL != replace_to)
	{
		zbx_replace_string(data, token->token.l, &token->token.r, replace_to);
		zbx_free(replace_to);
	}

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: process_user_macro_token                                         *
 *                                                                            *
 * Purpose: expand discovery macro in user macro context                      *
 *                                                                            *
 * Parameters: data      - [IN/OUT] the expression containing lld macro       *
 *             token     - [IN/OUT] the token with user macro location data   *
 *             jp_row    - [IN] discovery data                                *
 *                                                                            *
 ******************************************************************************/
static void	process_user_macro_token(char **data, zbx_token_t *token, const struct zbx_json_parse *jp_row)
{
	int			force_quote;
	size_t			context_r;
	char			*context, *context_esc;
	zbx_token_user_macro_t	*macro = &token->data.user_macro;

	/* user macro without context, nothing to replace */
	if (0 == token->data.user_macro.context.l)
		return;

	force_quote = ('"' == (*data)[macro->context.l]);
	context = zbx_user_macro_unquote_context_dyn(*data + macro->context.l, macro->context.r - macro->context.l + 1);

	/* substitute_lld_macros() can't fail with only ZBX_TOKEN_LLD_MACRO flag set */
	substitute_lld_macros(&context, jp_row, ZBX_TOKEN_LLD_MACRO, NULL, NULL, 0);

	context_esc = zbx_user_macro_quote_context_dyn(context, force_quote);

	context_r = macro->context.r;
	zbx_replace_string(data, macro->context.l, &context_r, context_esc);

	token->token.r += context_r - macro->context.r;

	zbx_free(context_esc);
	zbx_free(context);
}

/******************************************************************************
 *                                                                            *
 * Function: validate_func_macro                                              *
 *                                                                            *
 * Purpose: checks if the function macro located at token contains supported  *
 *          macro                                                             *
 *                                                                            *
 * Parameters: expression  - [IN] the expression containing function macro    *
 *             token       - [IN] the token with function macro location data *
 *             func_macros - [IN] NULL terminated array of supported macro    *
 *                                names without enclosing brackets            *
 *                                                                            *
 * Return value: SUCCEED - the function macro is supported                    *
 *               FAIL    - the function macro is not supported                *
 *                                                                            *
 ******************************************************************************/
int	validate_func_macro(const char *expression, zbx_token_t *token, const char **func_macros)
{
	const char	*macro;
	size_t		len;

	if (NULL == func_macros)
		return SUCCEED;

	macro = expression + token->data.func_macro.macro.l + 1;

	for (; NULL != *func_macros; func_macros++)
	{
		len = strlen(*func_macros);

		if (0 != strncmp(macro, *func_macros, len))
			continue;

		if ('}' != macro[len] && (0 == isdigit(macro[len]) || '}' != macro[len + 1]))
			continue;

		return SUCCEED;
	}

	return FAIL;
}

/******************************************************************************
 *                                                                            *
 * Function: substitute_lld_macros                                            *
 *                                                                            *
 * Parameters: data   - [IN/OUT] pointer to a buffer                          *
 *             jp_row - [IN] discovery data                                   *
 *             flags  - [IN] ZBX_MACRO_ANY - all LLD macros will be resolved  *
 *                            without validation of the value type            *
 *                           ZBX_MACRO_NUMERIC - values for LLD macros should *
 *                            be numeric                                      *
 *                           ZBX_MACRO_SIMPLE - LLD macros, located in the    *
 *                            item key parameters in simple macros will be    *
 *                            resolved considering quotes.                    *
 *                            Flag ZBX_MACRO_NUMERIC doesn't affect these     *
 *                            macros.                                         *
 *                           ZBX_MACRO_FUNC - function macros will be         *
 *                            skipped (lld macros inside function macros will *
 *                            be ignored) for macros specified in func_macros *
 *                            array                                           *
 *             func_macros - [IN] an optional NULL terminated array of macros *
 *                            supporting functions. This array is used when   *
 *                            ZBX_MACRO_FUNC flag is specified to determine   *
 *                            if a function macro is supported.               *
 *             error  - [OUT] should be not NULL if ZBX_MACRO_NUMERIC flag is *
 *                            set                                             *
 *             max_error_len - [IN] the size of error buffer                  *
 *                                                                            *
 * Return value: Always SUCCEED if numeric flag is not set, otherwise SUCCEED *
 *               if all discovery macros resolved to numeric values,          *
 *               otherwise FAIL with an error message.                        *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 ******************************************************************************/
int	substitute_lld_macros(char **data, const struct zbx_json_parse *jp_row, int flags, const char **func_macros,
		char *error, size_t max_error_len)
{
	const char	*__function_name = "substitute_lld_macros";

	int		ret = SUCCEED, pos = 0;
	zbx_token_t	token;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() data:'%s'", __function_name, *data);

	while (SUCCEED == ret && SUCCEED == zbx_token_find(*data, pos, &token))
	{
		if (0 != (token.type & flags))
		{
			switch (token.type)
			{
				case ZBX_TOKEN_LLD_MACRO:
					ret = process_lld_macro_token(data, &token, flags, jp_row, error,
							max_error_len);
					pos = token.token.r;
					break;
				case ZBX_TOKEN_USER_MACRO:
					process_user_macro_token(data, &token, jp_row);
					pos = token.token.r;
					break;
				case ZBX_TOKEN_SIMPLE_MACRO:
					process_simple_macro_token(data, &token, jp_row);
					pos = token.token.r;
					break;
				case ZBX_TOKEN_FUNC_MACRO:
					/* LLD macros must be ignored inside supported function macros.       */
					/* For example when expanding lld macro {#LLDMACRO} with value "ABC"  */
					/* {{ITEM.VALUE}.regsub({#LLDMACRO})} should be translated to         */
					/*     {{ITEM.VALUE}.regsub({#LLDMACRO})}, but                        */
					/* {{ITEM.KEY}.regsub({#LLDMACRO})} should be translated to           */
					/*     {{ITEM.KEY}.regsub(ABC)}                                       */
					/* In the first case the expression contains valid function macro and */
					/* lld macros are not supported inside function macro parameters.     */
					/* In the second case {ITEM.KEY} macro does not support functions, so */
					/* it is not a valid function macro and is processed as a plain text, */
					/* expanding lld macros inside it.                                    */
					if (SUCCEED == validate_func_macro(*data, &token, func_macros))
						pos = token.token.r;
					break;
			}
		}

		pos++;
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s data:'%s'", __function_name, zbx_result_string(ret), *data);

	return ret;
}

typedef struct
{
	zbx_uint64_t			*hostid;
	DC_ITEM				*dc_item;
	const struct zbx_json_parse	*jp_row;
	int				macro_type;
}
replace_key_param_data_t;

/******************************************************************************
 *                                                                            *
 * Function: replace_key_param                                                *
 *                                                                            *
 * Comments: auxiliary function for substitute_key_macros()                   *
 *                                                                            *
 ******************************************************************************/
static int	replace_key_param_cb(const char *data, int key_type, int level, int num, int quoted, void *cb_data,
			char **param)
{
	replace_key_param_data_t	*replace_key_param_data = (replace_key_param_data_t *)cb_data;
	zbx_uint64_t			*hostid = replace_key_param_data->hostid;
	DC_ITEM				*dc_item = replace_key_param_data->dc_item;
	const struct zbx_json_parse	*jp_row = replace_key_param_data->jp_row;
	int				macro_type = replace_key_param_data->macro_type, ret = SUCCEED;

	ZBX_UNUSED(num);

	if (ZBX_KEY_TYPE_ITEM == key_type && 0 == level)
		return ret;

	if (NULL == strchr(data, '{'))
		return ret;

	*param = zbx_strdup(NULL, data);

	if (0 != level)
		unquote_key_param(*param);

	if (NULL == jp_row)
		substitute_simple_macros(NULL, NULL, NULL, NULL, hostid, NULL, dc_item, NULL,
				param, macro_type, NULL, 0);
	else
		substitute_lld_macros(param, jp_row, ZBX_MACRO_ANY, NULL, NULL, 0);

	if (0 != level)
	{
		if (FAIL == (ret = quote_key_param(param, quoted)))
			zbx_free(*param);
	}

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: substitute_key_macros                                            *
 *                                                                            *
 * Purpose: safely substitutes macros in parameters of an item key and OID    *
 *                                                                            *
 * Example:  key                     | macro  | result            | return    *
 *          -------------------------+--------+-------------------+---------  *
 *           echo.sh[{$MACRO}]       | a      | echo.sh[a]        | SUCCEED   *
 *           echo.sh[{$MACRO}]       | a\     | echo.sh[a\]       | SUCCEED   *
 *           echo.sh["{$MACRO}"]     | a      | echo.sh["a"]      | SUCCEED   *
 *           echo.sh["{$MACRO}"]     | a\     | undefined         | FAIL      *
 *           echo.sh[{$MACRO}]       |  a     | echo.sh[" a"]     | SUCCEED   *
 *           echo.sh[{$MACRO}]       |  a\    | undefined         | FAIL      *
 *           echo.sh["{$MACRO}"]     |  a     | echo.sh[" a"]     | SUCCEED   *
 *           echo.sh["{$MACRO}"]     |  a\    | undefined         | FAIL      *
 *           echo.sh[{$MACRO}]       | "a"    | echo.sh["\"a\""]  | SUCCEED   *
 *           echo.sh[{$MACRO}]       | "a"\   | undefined         | FAIL      *
 *           echo.sh["{$MACRO}"]     | "a"    | echo.sh["\"a\""]  | SUCCEED   *
 *           echo.sh["{$MACRO}"]     | "a"\   | undefined         | FAIL      *
 *           echo.sh[{$MACRO}]       | a,b    | echo.sh["a,b"]    | SUCCEED   *
 *           echo.sh[{$MACRO}]       | a,b\   | undefined         | FAIL      *
 *           echo.sh["{$MACRO}"]     | a,b    | echo.sh["a,b"]    | SUCCEED   *
 *           echo.sh["{$MACRO}"]     | a,b\   | undefined         | FAIL      *
 *           echo.sh[{$MACRO}]       | a]     | echo.sh["a]"]     | SUCCEED   *
 *           echo.sh[{$MACRO}]       | a]\    | undefined         | FAIL      *
 *           echo.sh["{$MACRO}"]     | a]     | echo.sh["a]"]     | SUCCEED   *
 *           echo.sh["{$MACRO}"]     | a]\    | undefined         | FAIL      *
 *           echo.sh[{$MACRO}]       | [a     | echo.sh["a]"]     | SUCCEED   *
 *           echo.sh[{$MACRO}]       | [a\    | undefined         | FAIL      *
 *           echo.sh["{$MACRO}"]     | [a     | echo.sh["[a"]     | SUCCEED   *
 *           echo.sh["{$MACRO}"]     | [a\    | undefined         | FAIL      *
 *           ifInOctets.{#SNMPINDEX} | 1      | ifInOctets.1      | SUCCEED   *
 *                                                                            *
 ******************************************************************************/
int	substitute_key_macros(char **data, zbx_uint64_t *hostid, DC_ITEM *dc_item, const struct zbx_json_parse *jp_row,
		int macro_type, char *error, size_t maxerrlen)
{
	const char			*__function_name = "substitute_key_macros";
	replace_key_param_data_t	replace_key_param_data;
	int				key_type, ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() data:'%s'", __function_name, *data);

	replace_key_param_data.hostid = hostid;
	replace_key_param_data.dc_item = dc_item;
	replace_key_param_data.jp_row = jp_row;
	replace_key_param_data.macro_type = macro_type;

	switch (macro_type)
	{
		case MACRO_TYPE_ITEM_KEY:
			key_type = ZBX_KEY_TYPE_ITEM;
			break;
		case MACRO_TYPE_SNMP_OID:
			key_type = ZBX_KEY_TYPE_OID;
			break;
		default:
			THIS_SHOULD_NEVER_HAPPEN;
			exit(EXIT_FAILURE);
	}

	ret = replace_key_params_dyn(data, key_type, replace_key_param_cb, &replace_key_param_data, error, maxerrlen);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s data:'%s'", __function_name, zbx_result_string(ret), *data);

	return ret;
}
