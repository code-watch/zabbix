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
 * Class containing methods for operations with the main part of administration settings.
 */
class CSettings extends CApiService {
	/**
	 * @var string
	 */
	protected $tableName = 'config';

	/**
	 * @var string
	 */
	protected $tableAlias = 'c';

	/**
	 * @var array
	 */
	private $output_fields = ['default_theme', 'search_limit', 'max_in_table', 'server_check_interval', 'work_period',
		'show_technical_errors', 'history_period', 'period_default', 'max_period', 'severity_color_0',
		'severity_color_1', 'severity_color_2', 'severity_color_3', 'severity_color_4', 'severity_color_5',
		'severity_name_0', 'severity_name_1', 'severity_name_2', 'severity_name_3', 'severity_name_4',
		'severity_name_5', 'custom_color', 'ok_period', 'blink_period', 'problem_unack_color', 'problem_ack_color',
		'ok_unack_color', 'ok_ack_color', 'problem_unack_style', 'problem_ack_style', 'ok_unack_style',
		'ok_ack_style', 'refresh_unsupported', 'discovery_groupid', 'default_inventory_mode', 'alert_usrgrpid',
		'snmptrap_logging', 'login_attempts', 'login_block', 'session_name', 'validate_uri_schemes',
		'uri_valid_schemes', 'x_frame_options', 'connect_timeout', 'socket_timeout', 'media_type_test_timeout',
		'script_timeout', 'item_test_timeout'];

	/**
	 * Get settings paramters
	 * 
	 * @param array $options
	 *
	 * @throws APIException if the input is invalid.
	 *
	 * @return array
	 */
	public function get(array $options): array {
		$api_input_rules = ['type' => API_OBJECT, 'fields' => [
			'output' =>	['type' => API_OUTPUT, 'in' => implode(',', $this->output_fields), 'default' => API_OUTPUT_EXTEND]
		]];
		if (!CApiInputValidator::validate($api_input_rules, $options, '/', $error)) {
			self::exception(ZBX_API_ERROR_PARAMETERS, $error);
		}

		if (self::$userData['type'] != USER_TYPE_SUPER_ADMIN) {
			return [];
		}

		if ($options['output'] === API_OUTPUT_EXTEND) {
			$options['output'] = $this->output_fields;
		}

		$db_settings = [];

		$result = DBselect($this->createSelectQuery($this->tableName(), $options));
		while ($row = DBfetch($result)) {
			$db_settings[] = $row;
		}
		$db_settings = $this->unsetExtraFields($db_settings, ['configid'], []);

		return $db_settings[0];
	}

	/**
	 * Update settings paramters
	 * 
	 * @param array  $settings
	 *
	 * @return bool
	 */
	public function update(array $settings): array {
		$this->validateUpdate($settings, $db_settings);

		$upd_config = [];

		// strings
		$field_names = ['default_theme', 'work_period', 'history_period', 'period_default', 'max_period',
			'severity_color_0', 'severity_color_1', 'severity_color_2', 'severity_color_3', 'severity_color_4',
			'severity_color_5', 'severity_name_0', 'severity_name_1', 'severity_name_2', 'severity_name_3',
			'severity_name_4', 'severity_name_5', 'ok_period', 'blink_period', 'problem_unack_color',
			'problem_ack_color', 'ok_unack_color', 'ok_ack_color', 'refresh_unsupported', 'login_block', 'session_name',
			'uri_valid_schemes', 'x_frame_options', 'connect_timeout', 'socket_timeout', 'media_type_test_timeout',
			'script_timeout', 'item_test_timeout'];
		foreach ($field_names as $field_name) {
			if (array_key_exists($field_name, $settings) && $settings[$field_name] !== $db_settings[$field_name]) {
				$upd_config[$field_name] = $settings[$field_name];
			}
		}

		// integers
		$field_names = ['search_limit', 'max_in_table', 'server_check_interval', 'show_technical_errors',
			'custom_color', 'problem_unack_style', 'problem_ack_style', 'ok_unack_style', 'ok_ack_style',
			'discovery_groupid', 'default_inventory_mode', 'alert_usrgrpid', 'snmptrap_logging', 'login_attempts',
			'validate_uri_schemes'];
		foreach ($field_names as $field_name) {
			if (array_key_exists($field_name, $settings) && $settings[$field_name] != $db_settings[$field_name]) {
				$upd_config[$field_name] = $settings[$field_name];
			}
		}

		if ($upd_config) {
			DB::update('config', [
				'values' => $upd_config,
				'where' => ['configid' => $db_settings['configid']]
			]);
		}

		$this->addAuditBulk(AUDIT_ACTION_UPDATE, AUDIT_RESOURCE_SETTINGS,
			[['configid' => $db_settings['configid']] + $settings], [$db_settings['configid'] => $db_settings]
		);

		return array_keys($upd_config);
	}

	/**
	 * Validate updated settings parameters
	 * 
	 * @param array  $settings
	 * @param array  $db_settings
	 *
	 * @throws APIException if the input is invalid.
	 */
	protected function validateUpdate(array &$settings, array &$db_settings = null) {
		$api_input_rules = ['type' => API_OBJECT, 'flags' => API_NOT_EMPTY, 'fields' => [
			'default_theme' =>				['type' => API_STRING_UTF8,
												'in' => implode(',', array_keys(APP::getThemes()))],
			'search_limit' =>				['type' => API_INT32, 'in' => '1:999999'],
			'max_in_table' =>				['type' => API_INT32, 'in' => '1:99999'],
			'server_check_interval' =>		['type' => API_INT32, 'in' => '0,'.SERVER_CHECK_INTERVAL],
			'work_period' =>				['type' => API_TIME_PERIOD],
			'show_technical_errors' =>		['type' => API_INT32, 'in' => '0,1'],
			'history_period' =>				['type' => API_TIME_UNIT,
												'in' => implode(':', [SEC_PER_DAY, 7 * SEC_PER_DAY])],
			'period_default' =>				['type' => API_TIME_UNIT, 'flags' => API_TIME_UNIT_WITH_YEAR,
												'in' => implode(':', [SEC_PER_MIN, 10 * SEC_PER_YEAR])],
			'max_period' =>					['type' => API_TIME_UNIT, 'flags' => API_TIME_UNIT_WITH_YEAR,
												'in' => implode(':', [SEC_PER_YEAR, 10 * SEC_PER_YEAR])],
			'severity_color_0' =>			['type' => API_COLOR, 'flags' => API_NOT_EMPTY],
			'severity_color_1' =>			['type' => API_COLOR, 'flags' => API_NOT_EMPTY],
			'severity_color_2' =>			['type' => API_COLOR, 'flags' => API_NOT_EMPTY],
			'severity_color_3' =>			['type' => API_COLOR, 'flags' => API_NOT_EMPTY],
			'severity_color_4' =>			['type' => API_COLOR, 'flags' => API_NOT_EMPTY],
			'severity_color_5' =>			['type' => API_COLOR, 'flags' => API_NOT_EMPTY],
			'severity_name_0' =>			['type' => API_STRING_UTF8, 'length' => 32],
			'severity_name_1' =>			['type' => API_STRING_UTF8, 'length' => 32],
			'severity_name_2' =>			['type' => API_STRING_UTF8, 'length' => 32],
			'severity_name_3' =>			['type' => API_STRING_UTF8, 'length' => 32],
			'severity_name_4' =>			['type' => API_STRING_UTF8, 'length' => 32],
			'severity_name_5' =>			['type' => API_STRING_UTF8, 'length' => 32],
			'custom_color' =>				['type' => API_INT32, 'in' => EVENT_CUSTOM_COLOR_DISABLED.','.
												EVENT_CUSTOM_COLOR_ENABLED],
			'ok_period' =>					['type' => API_TIME_UNIT],
			'blink_period' =>				['type' => API_TIME_UNIT],
			'problem_unack_color' =>		['type' => API_COLOR, 'flags' => API_NOT_EMPTY],
			'problem_ack_color' =>			['type' => API_COLOR, 'flags' => API_NOT_EMPTY],
			'ok_unack_color' =>				['type' => API_COLOR, 'flags' => API_NOT_EMPTY],
			'ok_ack_color' =>				['type' => API_COLOR, 'flags' => API_NOT_EMPTY],
			'problem_unack_style' =>		['type' => API_INT32, 'in' => '0,1'],
			'problem_ack_style' =>			['type' => API_INT32, 'in' => '0,1'],
			'ok_unack_style' =>				['type' => API_INT32, 'in' => '0,1'],
			'ok_ack_style' =>				['type' => API_INT32, 'in' => '0,1'],
			'refresh_unsupported' =>		['type' => API_TIME_UNIT],
			'discovery_groupid' =>			['type' => API_ID],
			'default_inventory_mode' =>		['type' => API_INT32, 'in' => HOST_INVENTORY_DISABLED.','.
												HOST_INVENTORY_MANUAL.','.HOST_INVENTORY_AUTOMATIC],
			'alert_usrgrpid' =>				['type' => API_ID],
			'snmptrap_logging' =>			['type' => API_INT32, 'in' => '0,1'],
			'login_attempts' =>				['type' => API_INT32, 'in' => '1:32'],
			'login_block' =>				['type' => API_TIME_UNIT, 'in' => implode(':', [30, SEC_PER_HOUR])],
			'session_name' =>				['type' => API_STRING_UTF8, 'length' => 32],
			'validate_uri_schemes' =>		['type' => API_INT32, 'in' => '0,1'],
			'uri_valid_schemes' =>			['type' => API_STRING_UTF8, 'length' => 255],
			'x_frame_options' =>			['type' => API_STRING_UTF8, 'length' => 255],
			'connect_timeout' =>			['type' => API_TIME_UNIT, 'in' => '1:30'],
			'socket_timeout' =>				['type' => API_TIME_UNIT, 'in' => '1:300'],
			'media_type_test_timeout' =>	['type' => API_TIME_UNIT, 'in' => '1:300'],
			'script_timeout' =>				['type' => API_TIME_UNIT, 'in' => '1:300'],
			'item_test_timeout' =>			['type' => API_TIME_UNIT, 'in' => '1:300']
		]];
		if (!CApiInputValidator::validate($api_input_rules, $settings, '/', $error)) {
			self::exception(ZBX_API_ERROR_PARAMETERS, $error);
		}

		// Check permissions.
		if (self::$userData['type'] != USER_TYPE_SUPER_ADMIN) {
			self::exception(ZBX_API_ERROR_PERMISSIONS, _('No permissions to referred object or it does not exist!'));
		}

		if (array_key_exists('discovery_groupid', $settings)) {
			$db_hstgrp_exists = API::HostGroup()->get([
				'countOutput' => true,
				'groupids' => $settings['discovery_groupid'],
				'filter' => ['flags' => ZBX_FLAG_DISCOVERY_NORMAL],
				'editable' => true
			]);
			if (!$db_hstgrp_exists) {
				self::exception(ZBX_API_ERROR_PARAMETERS, _s('Host group with ID "%1$s" is not available.',
				$settings['discovery_groupid']));
			}
		}

		if (array_key_exists('alert_usrgrpid', $settings)) {
			$db_usrgrp_exists = DB::select('usrgrp', [
				'countOutput' => true,
				'usrgrpids' => $settings['alert_usrgrpid']
			]);
			if (!$db_usrgrp_exists[0]['rowscount']) {
				self::exception(ZBX_API_ERROR_PARAMETERS, _s('User group with ID "%1$s" is not available.',
					$settings['alert_usrgrpid']));
			}
		}

		$db_settings = DB::select('config', ['output' => ['configid'] + $this->output_fields])[0];
	}
}