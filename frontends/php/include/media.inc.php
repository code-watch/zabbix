<?php
/*
** Zabbix
** Copyright (C) 2001-2013 Zabbix SIA
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


function media_type2str($type = null) {
	$mediaTypes = array(
		MEDIA_TYPE_EMAIL => _('Email'),
		MEDIA_TYPE_EXEC => _('Script'),
		MEDIA_TYPE_SMS => _('SMS'),
		MEDIA_TYPE_JABBER => _('Jabber'),
		MEDIA_TYPE_EZ_TEXTING => _('Ez Texting'),
		MEDIA_TYPE_REMEDY => _('Remedy Service')
	);

	if ($type === null) {
		natsort($mediaTypes);

		return $mediaTypes;
	}
	elseif (isset($mediaTypes[$type])) {
		return $mediaTypes[$type];
	}
	else {
		return _('Unknown');
	}
}
/**
 * Get translated label depending on Media Type field type.
 *
 * @param string $field
 * @param string $type
 *
 * @return string
 */
function getMediaTypeLabel($field, $type) {
	switch ($field) {
		case 'smtp_server':
			switch ($type) {
				case MEDIA_TYPE_REMEDY:
					return _('Remedy Service URL');
				case MEDIA_TYPE_EMAIL:
				default:
					return _('SMTP server');
			}

		case 'smtp_helo':
			switch ($type) {
				case MEDIA_TYPE_REMEDY:
					return _('Proxy');
				case MEDIA_TYPE_EMAIL:
				default:
					return _('SMTP helo');
			}

		case 'exec_path':
			switch ($type) {
				case MEDIA_TYPE_EZ_TEXTING:
					return _('Message text limit');
				case MEDIA_TYPE_REMEDY:
					return _('Company name');
				case MEDIA_TYPE_EXEC:
				default:
					return _('Script name');

			}

		case 'username':
			switch ($type) {
				case MEDIA_TYPE_JABBER:
					return _('Jabber identifier');
				default:
					return _('Username');
			}

		default:
			return $field;
	}
}
