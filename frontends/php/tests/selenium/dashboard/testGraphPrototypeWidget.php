<?php
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

require_once dirname(__FILE__).'/../../include/CWebTest.php';

/**
 * @backup widget
 * @backup profiles
 */
class testGraphPrototypeWidget extends CWebTest {

	/**
	 * SQL query to get widget and widget_field tables to compare hash values, but without widget_fieldid
	 * because it can change.
	 */
	private $sql = 'SELECT wf.widgetid, wf.type, wf.name, wf.value_int, wf.value_str, wf.value_groupid, wf.value_hostid,'.
			' wf.value_itemid, wf.value_graphid, wf.value_sysmapid, w.widgetid, w.dashboardid, w.type, w.name, w.x, w.y,'.
			' w.width, w.height'.
			' FROM widget_field wf'.
			' INNER JOIN widget w'.
			' ON w.widgetid=wf.widgetid ORDER BY wf.widgetid, wf.name, wf.value_int, wf.value_str, wf.value_groupid,'.
			' wf.value_itemid, wf.value_graphid';

	const DASHBOARD_ID = 105;
	const SCREENSHOT_DASHBOARD_ID = 106;

	private static $previous_widget_name = 'Graph prototype widget for update';

	public static function getWidgetData() {
		return [
			[
				[
					'expected' => TEST_GOOD,
					'fields' => [
						'Type' => 'Graph prototype'
					],
					'Graph prototype' => [
						'values' => 'testFormGraphPrototype1',
						'context' => ['groupid' => 'Zabbix servers', 'hostid' => 'Simple form test host']
					]
				]
			],
			[
				[
					'expected' => TEST_GOOD,
					'fields' => [
						'Type' => 'Graph prototype',
						'Name' => 'Simple graph prototype'.microtime(),
						'Source' => 'Simple graph prototype'
					],
					'Item prototype' => [
						'values' => 'testFormItemPrototype1',
						'context' => ['groupid' => 'Zabbix servers', 'hostid' => 'Simple form test host']
					],
					'show_header' => false
				]
			],
			[
				[
					'expected' => TEST_GOOD,
					'fields' => [
						'Type' => 'Graph prototype',
						'Name' => 'Graph prototype widget with all possible fields filled'.microtime(),
						'Refresh interval' => 'No refresh',
						'Source' => 'Simple graph prototype',
						'Show legend' => true,
						'Dynamic item' => true,
						'Columns' => '3',
						'Rows' => '2'
					],
					'Item prototype' => 'testFormItemPrototype2'
				]
			],
			[
				[
					'expected' => TEST_BAD,
					'fields' => [
						'Type' => 'Graph prototype',
						'Source' => 'Graph prototype'
					],
					'error' => ['Invalid parameter "Graph prototype": cannot be empty.']
				]
			],
			[
				[
					'expected' => TEST_BAD,
					'fields' => [
						'Type' => 'Graph prototype',
						'Source' => 'Simple graph prototype'
					],
					'error' => ['Invalid parameter "Item prototype": cannot be empty.']
				]
			],
			[
				[
					'expected' => TEST_BAD,
					'fields' => [
						'Type' => 'Graph prototype',
						'Source' => 'Graph prototype',
						'Columns' => '0',
						'Rows' => '0'
					],
					'Graph prototype' => 'testFormGraphPrototype1',
					'error' => [
						'Invalid parameter "Columns": value must be one of 1-24.',
						'Invalid parameter "Rows": value must be one of 1-16.'
					]
				]
			],
						[
				[
					'expected' => TEST_BAD,
					'fields' => [
						'Type' => 'Graph prototype',
						'Source' => 'Graph prototype',
						'Columns' => '25',
						'Rows' => '17'
					],
					'Graph prototype' => 'testFormGraphPrototype1',
					'error' => [
						'Invalid parameter "Columns": value must be one of 1-24.',
						'Invalid parameter "Rows": value must be one of 1-16.'
					]
				]
			]
		];
	}

	/**
	 * Test for checking new Graph prototype widget creation.
	 *
	 * @dataProvider getWidgetData
	 */
	public function testGraphPrototypeWidget_Create($data) {
		$this->checkGraphPrototypeWidget($data);
	}

	/**
	 * Test for checking existing Graph prototype widget update.
	 *
	 * @dataProvider getWidgetData
	 */
	public function testGraphPrototypeWidget_Update($data) {
		$this->checkGraphPrototypeWidget($data, true);
	}

	/**
	 * Test for checking Graph prototype widget update without any changes.
	 */
	public function testGraphPrototypeWidget_SimpleUpdate() {
		$this->checkDataUnchanged('Apply', true);
	}

	/**
	 * Test for checking Graph prototype creation cancelling.
	 */
	public function testGraphPrototypeWidget_CancelCreate() {
		$this->checkDataUnchanged('Cancel', false, true);
	}

	/**
	 * Test for checking Graph prototype cancelling form changes.
	 */
	public function testGraphPrototypeWidget_CancelChanges() {
		$this->checkDataUnchanged('Cancel', true, true);
	}

	/**
	 * Test for checking Graph prototype widget cancelling without making any changes.
	 */
	public function testGraphPrototypeWidget_CancelNoChanges() {
		$this->checkDataUnchanged('Cancel', true);
	}

	/**
	 * Test for checking delete of Graph prototype widget.
	 */
	public function testGraphPrototypeWidget_Delete() {
		$name = 'Graph prototype widget for delete';

		$this->page->login()->open('zabbix.php?action=dashboard.view&dashboardid='.self::DASHBOARD_ID);
		$dashboard = CDashboardElement::find()->one();
		$widget = $dashboard->edit()->getWidget($name);
		$this->assertEquals(true, $widget->isEditable());
		$dashboard->deleteWidget($name);

		$dashboard->save();
		$this->page->waitUntilReady();
		$message = CMessageElement::find()->waitUntilPresent()->one();
		$this->assertTrue($message->isGood());
		$this->assertEquals('Dashboard updated', $message->getTitle());

		// Check that widget is not present on dashboard and in DB.
		$this->assertTrue(!$dashboard->getWidget($name, false)->isValid());
		$sql = 'SELECT * FROM widget_field wf LEFT JOIN widget w ON w.widgetid=wf.widgetid'.
				' WHERE w.name='.zbx_dbstr($name);
		$this->assertEquals(0, CDBHelper::getCount($sql));
	}

	/**
	 * Test for comparing widgets form screenshot.
	 */
	public function testGraphPrototypeWidget_FormScreenshot() {
		$this->page->login()->open('zabbix.php?action=dashboard.view&dashboardid='.self::SCREENSHOT_DASHBOARD_ID);
		$dashboard = CDashboardElement::find()->one();
		$form = $dashboard->edit()->addWidget()->asForm();
		if ($form->getField('Type')->getText() !== 'Graph prototype') {
			$form->fill(['Type' => 'Graph prototype']);
			$form->waitUntilReloaded();
		}
		$this->page->removeFocus();
		sleep(1);
		$dialog = $this->query('id:overlay_dialogue')->one();
		$this->assertScreenshot($dialog);
	}

	public static function getWidgetScreenshotData() {
		return [
			[
				[
					'screenshot_id' => 'default'
				]
			],
			[
				[
					'fields' => [
						'Columns' => '3',
						'Rows' => '1'
					],
					'screenshot_id' => '3x1'
				]
			],
			[
				[
					'fields' => [
						'Columns' => '2',
						'Rows' => '2'
					],
					'screenshot_id' => '2x2'
				]
			],
			[
				[
					'fields' => [
						'Columns' => '16',
						'Rows' => '1'
					],
					'screenshot_id' => '16x1'
				]
			],
			[
				[
					'fields' => [
						'Columns' => '16',
						'Rows' => '2'
					],
					'screenshot_id' => '16x2'
				]
			],
			[
				[
					'fields' => [
						'Columns' => '16',
						'Rows' => '3'
					],
					'screenshot_id' => 'stub16x3'
				]
			],
			[
				[
					'fields' => [
						'Columns' => '17',
						'Rows' => '2'
					],
					'screenshot_id' => 'stub17x2'
				]
			]
		];
	}

	/**
	 * Test for comparing widgets grid screenshots.
	 * @backup widget
	 * @dataProvider getWidgetScreenshotData
	 */
	public function testGraphPrototypeWidget_GridScreenshots($data) {
		$this->page->login()->open('zabbix.php?action=dashboard.view&dashboardid='.self::SCREENSHOT_DASHBOARD_ID);
		$dashboard = CDashboardElement::find()->one();
		$form = $dashboard->edit()->addWidget()->asForm();
		$widget = [
			'Name' => 'Screenshot Widget',
			'Graph prototype' => 'testFormGraphPrototype1'
		];
		if ($form->getField('Type')->getText() !== 'Graph prototype') {
			$form->fill(['Type' => 'Graph prototype']);
			$form->waitUntilReloaded();
		}
		$form->fill($widget);
		if (array_key_exists('fields', $data)){
			$form->fill($data['fields']);
		}
		$form->submit();
		$dashboard->getWidget($widget['Name']);
		$dashboard->save();
		$screenshot_area = $this->query('class:dashbrd-grid-container')->one();
		$this->assertScreenshot($screenshot_area, $data['screenshot_id']);
	}

	private function checkGraphPrototypeWidget($data, $update = false) {
		$this->page->login()->open('zabbix.php?action=dashboard.view&dashboardid='.self::DASHBOARD_ID);
		$dashboard = CDashboardElement::find()->one();
		$old_widget_count = $dashboard->getWidgets()->count();

		$form = $update
			? $dashboard->getWidget(self::$previous_widget_name)->edit()
			: $dashboard->edit()->addWidget()->asForm();

		if (array_key_exists('show_header', $data)) {
			$form->query('xpath:.//input[@id="show_header"]')->asCheckbox()->one()->fill($data['show_header']);
		}
		$form->fill($data['fields']);

		if (array_key_exists('Graph prototype', $data) || array_key_exists('Item prototype', $data)){
			$type = CTestArrayHelper::get($data['fields'], 'Source') === 'Simple graph prototype'
						? 'Item prototype' : 'Graph prototype';
			$field = $form->getField($type);

			if (is_array($data[$type])){
				$field->setFillMode(CMultiselectElement::MODE_SELECT)->fill($data[$type]);
			}
			else {
				$field->fill($data[$type]);
			}
		}
		else {
			$form->query('xpath:.//div[@id="graphid" | @id="itemid"]')->asMultiselect()->one()->clear();
		}

		$values = $form->getFields()->asValues();
		$form->submit();

		switch ($data['expected']) {
			case TEST_GOOD:
				// Introduce name for finding saved widget in DB.
				if ($update && array_key_exists('Name', $data['fields'])) {
					$db_name = $data['fields']['Name'];
				}
				elseif ($update && !array_key_exists('Name', $data['fields'])) {
					$db_name = self::$previous_widget_name;
				}
				else {
					$db_name = '';
				}
				// Make sure that the widget is present before saving the dashboard.
				if (!array_key_exists('Name', $data['fields'])) {
					$default_header = $update ? self::$previous_widget_name
							: $data[$type]['context']['hostid'].': '.$data[$type]['values'];
					$data['fields']['Name'] = $default_header;
				}
				$dashboard->getWidget($data['fields']['Name']);
				$dashboard->save();

				// Check that Dashboard has been saved and that widget has been added.
				$this->checkDashboardUpdateMessage();
				$count = $update ? $old_widget_count : $old_widget_count + 1;
				$this->assertEquals($count, $dashboard->getWidgets()->count());

				// Check that widget is saved in DB.
				$db_count = CDBHelper::getCount('SELECT * FROM widget WHERE dashboardid ='.
						self::DASHBOARD_ID.' AND  name ='.zbx_dbstr($db_name));
				$this->assertEquals(1, $db_count);

				// Verify widget content
				$widget = $dashboard->getWidget($data['fields']['Name']);
				$this->assertTrue($widget->getContent()->isValid());

				// Compare placeholders count in data and created widget.
				$expected_placeholders_count =
					(CTestArrayHelper::get($data['fields'], 'Columns') && CTestArrayHelper::get($data['fields'], 'Rows'))
					? $data['fields']['Columns'] * $data['fields']['Rows']
					: 2;
				$placeholders_count = $widget->query('class:dashbrd-grid-iterator-placeholder')->count();
				$this->assertEquals($expected_placeholders_count, $placeholders_count);
				// Check Dynamic item setting on Dashboard.
				if (CTestArrayHelper::get($data['fields'], 'Dynamic item')){
					$this->assertTrue($dashboard->getControls()->query('xpath://form[@aria-label = '.
						'"Main filter"]')->one()->isPresent());
				}
				// Check widget form fields and values.
				$this->assertEquals($values, $widget->edit()->getFields()->asValues());

				// Write widget name to variable to use it in next Update test case.
				if ($update) {
					self::$previous_widget_name = CTestArrayHelper::get($data, 'fields.Name', 'Graph prototype widget for update');
				}
				break;
			case TEST_BAD:
				$message = $form->getOverlayMessage();
				$this->assertTrue($message->isBad());
				$count = count($data['error']);
				$message->query('xpath:./div[@class="msg-details"]/ul/li['.$count.']')->waitUntilPresent();
				$this->assertEquals($count, $message->getLines()->count());

				foreach ($data['error'] as $error) {
					$this->assertTrue($message->hasLine($error));
				}
				break;
		}
	}

	/**
	 * Function for checking editing widget form without changes.
	 *
	 * @param string $action	name of button tested
	 * @param boolean $update	is this updating of existing widget
	 * @param boolean $changes	are the any changes made in widget form
	 */
	private function checkDataUnchanged($action, $update = false, $changes = false) {
		$initial_values = CDBHelper::getHash($this->sql);
		$this->page->login()->open('zabbix.php?action=dashboard.view&dashboardid='.self::DASHBOARD_ID);
		$dashboard = CDashboardElement::find()->one();

		$form = $update
			? $dashboard->getWidget(self::$previous_widget_name)->edit()
			: $dashboard->edit()->addWidget()->asForm();

		if ($update) {
			$original_values = $form->getFields()->asValues();
		}

		$dialog = $this->query('id:overlay_dialogue')->one();

		if ($changes) {
			$form->fill([
					'Type' => 'Graph prototype',
					'Name' => 'Name for Cancelling',
					'Refresh interval' => 'No refresh',
					'Source' => 'Simple graph prototype',
					'Item prototype' => 'testFormItemPrototype2',
					'Show legend' => false,
					'Dynamic item' => true,
					'Columns' => '3',
					'Rows' => '2'
				]);
		}

		$dialog->query('button', $action)->one()->click();
		$this->page->waitUntilReady();

		if ($update) {
			$dashboard->getWidget(self::$previous_widget_name);
		}

		$dashboard->save();
		// Check that Dashboard has been saved and that there are no changes made to the widgets.
		$this->checkDashboardUpdateMessage();

		if ($update) {
			$new_values = $dashboard->getWidget(self::$previous_widget_name)->edit()->getFields()->asValues();
			$this->assertEquals($original_values, $new_values);
		}

		$this->assertEquals($initial_values, CDBHelper::getHash($this->sql));
	}

	/**
	 * Check dashboard update message.
	 */
	private function checkDashboardUpdateMessage() {
		$message = CMessageElement::find()->waitUntilVisible()->one();
		$this->assertTrue($message->isGood());
		$this->assertEquals('Dashboard updated', $message->getTitle());
	}
}
