<?php declare(strict_types=1);
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


class CControllerApplicationEnable extends CController {

	protected function checkInput() {
		$fields = [
			'applicationids' => 'required|array_db applications.applicationid'
		];

		$ret = $this->validateInput($fields);

		if (!$ret) {
			$this->setResponse(new CControllerResponseFatal());
		}

		return $ret;
	}

	protected function checkPermissions() {
		if ($this->getUserType() < USER_TYPE_ZABBIX_ADMIN) {
			return false;
		}

		return (bool) API::Application()->get([
			'output' => [],
			'applicationids' => $this->getInput('applicationid'),
			'editable' => true
		]);
	}

	protected function doAction() {
		$db_items = API::Item()->get([
			'output' => ['itemid'],
			'applicationids' => $this->getInput('applicationids')
		]);

		$items = [];
		foreach ($db_items as $db_item) {
			$items[] = [
				'itemid' => $db_item['itemid'],
				'status' => ITEM_STATUS_ACTIVE
			];
		}

		$result = (bool) API::Item()->update($items);

		$updated = count($items);

		$response = new CControllerResponseRedirect((new CUrl('zabbix.php'))
			->setArgument('action', 'application.list')
			->setArgument('page', CPagerHelper::loadPage('application.list', null))
		);

		if ($result) {
			$response->setFormData(['uncheck' => '1']);
			$response->setMessageOk(_n('Item enabled', 'Items enabled', $updated));
		}
		else {
			$response->setMessageError(_n('Cannot enable item', 'Cannot enable items', $updated));
		}

		$this->setResponse($response);
	}
}
