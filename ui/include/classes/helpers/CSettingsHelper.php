<?php declare(strict_types = 1);
/*
** Zabbix
** Copyright (C) 2001-2020 Zabbix SIA
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


/**
 * A class for accessing once loaded parameters of Settings API object.
 */
class CSettingsHelper extends CConfigGeneralHelper {

	public const ALERT_USRGRPID = 'alert_usrgrpid';
	public const BLINK_PERIOD = 'blink_period';
	public const CONNECT_TIMEOUT = 'connect_timeout';
	public const CUSTOM_COLOR = 'custom_color';
	public const DEFAULT_INVENTORY_MODE = 'default_inventory_mode';
	public const DEFAULT_THEME = 'default_theme';
	public const DISCOVERY_GROUPID = 'discovery_groupid';
	public const HISTORY_PERIOD = 'history_period';
	public const ITEM_TEST_TIMEOUT = 'item_test_timeout';
	public const LOGIN_ATTEMPTS = 'login_attempts';
	public const LOGIN_BLOCK = 'login_block';
	public const MAX_IN_TABLE = 'max_in_table';
	public const MAX_PERIOD = 'max_period';
	public const MEDIA_TYPE_TEST_TIMEOUT = 'media_type_test_timeout';
	public const OK_ACK_COLOR = 'ok_ack_color';
	public const OK_ACK_STYLE = 'ok_ack_style';
	public const OK_PERIOD = 'ok_period';
	public const OK_UNACK_COLOR = 'ok_unack_color';
	public const OK_UNACK_STYLE = 'ok_unack_style';
	public const PERIOD_DEFAULT = 'period_default';
	public const PROBLEM_ACK_COLOR = 'problem_ack_color';
	public const PROBLEM_ACK_STYLE = 'problem_ack_style';
	public const PROBLEM_UNACK_COLOR = 'problem_unack_color';
	public const PROBLEM_UNACK_STYLE = 'problem_unack_style';
	public const REFRESH_UNSUPPORTED = 'refresh_unsupported';
	public const SCRIPT_TIMEOUT = 'script_timeout';
	public const SEARCH_LIMIT = 'search_limit';
	public const SERVER_CHECK_INTERVAL = 'server_check_interval';
	public const SESSION_NAME = 'session_name';
	public const SEVERITY_COLOR_0 = 'severity_color_0';
	public const SEVERITY_COLOR_1 = 'severity_color_1';
	public const SEVERITY_COLOR_2 = 'severity_color_2';
	public const SEVERITY_COLOR_3 = 'severity_color_3';
	public const SEVERITY_COLOR_4 = 'severity_color_4';
	public const SEVERITY_COLOR_5 = 'severity_color_5';
	public const SEVERITY_NAME_0 = 'severity_name_0';
	public const SEVERITY_NAME_1 = 'severity_name_1';
	public const SEVERITY_NAME_2 = 'severity_name_2';
	public const SEVERITY_NAME_3 = 'severity_name_3';
	public const SEVERITY_NAME_4 = 'severity_name_4';
	public const SEVERITY_NAME_5 = 'severity_name_5';
	public const SHOW_TECHNICAL_ERRORS = 'show_technical_errors';
	public const SNMPTRAP_LOGGING = 'snmptrap_logging';
	public const SOCKET_TIMEOUT = 'socket_timeout';
	public const URI_VALID_SCHEMES = 'uri_valid_schemes';
	public const VALIDATE_URI_SCHEMES = 'validate_uri_schemes';
	public const WORK_PERIOD = 'work_period';
	public const X_FRAME_OPTIONS = 'x_frame_options';

	/**
	 * @inheritdoc
	 */
	protected static function loadParams(): void {
		if (!self::$params) {
			self::$params = API::Settings()->get(['output' => 'extend']);
		}
	}
}