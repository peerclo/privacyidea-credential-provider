/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
**
** Copyright	2012 Dominik Pretzsch
**				2017 NetKnights GmbH
**
** Author		Dominik Pretzsch
**				Nils Behlen
**
**    Licensed under the Apache License, Version 2.0 (the "License");
**    you may not use this file except in compliance with the License.
**    You may obtain a copy of the License at
**
**        http://www.apache.org/licenses/LICENSE-2.0
**
**    Unless required by applicable law or agreed to in writing, software
**    distributed under the License is distributed on an "AS IS" BASIS,
**    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
**    See the License for the specific language governing permissions and
**    limitations under the License.
**
** * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef WIN32_NO_STATUS
#include <ntstatus.h>
#define WIN32_NO_STATUS
#endif

#include "CCredential.h"
#include "Configuration.h"
#include "Logger.h"
#include "json.hpp"
#include <string>
#include <thread>
#include <future>
#include <sstream>

using namespace std;

CCredential::CCredential(std::shared_ptr<Configuration> c) : _config(c), _util(_config)//, _privacyIDEA(_config->piconfig)
{
	_cRef = 1;
	_pCredProvCredentialEvents = nullptr;

	DllAddRef();

	_dwComboIndex = 0;

	ZERO(_rgCredProvFieldDescriptors);
	ZERO(_rgFieldStatePairs);
	ZERO(_rgFieldStrings);

	_privacyIDEA = PrivacyIDEA(_config->piconfig);
}

CCredential::~CCredential()
{
	_util.Clear(_rgFieldStrings, _rgCredProvFieldDescriptors, this, NULL, CLEAR_FIELDS_ALL_DESTROY);
	DllRelease();
}

// Initializes one credential with the field information passed in.
// Set the value of the SFI_USERNAME field to pwzUsername.
// Optionally takes a password for the SetSerialization case.
HRESULT CCredential::Initialize(
	__in const CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR* rgcpfd,
	__in const FIELD_STATE_PAIR* rgfsp,
	__in_opt PWSTR user_name,
	__in_opt PWSTR domain_name,
	__in_opt PWSTR password
)
{
	wstring wstrUsername, wstrDomainname, wstrPassword;

	if (NOT_EMPTY(user_name))
		wstrUsername = wstring(user_name);

	if (NOT_EMPTY(domain_name))
		wstrDomainname = wstring(domain_name);

	if (NOT_EMPTY(password))
		wstrPassword = wstring(password);


#ifdef _DEBUG
	DebugPrint(__FUNCTION__);
	DebugPrint(L"Username from provider: " + (wstrUsername.empty() ? L"empty" : wstrUsername));
	DebugPrint(L"Domain from provider: " + (wstrDomainname.empty() ? L"empty" : wstrDomainname));
	if (_config->logSensitive)
		DebugPrint(L"Password from provider: " + (wstrPassword.empty() ? L"empty" : wstrPassword));
#endif
	HRESULT hr = S_OK;

	if (!wstrUsername.empty())
	{
		DebugPrint("Copying user_name to credential");
		_config->credential.username = wstrUsername;
	}

	if (!wstrDomainname.empty())
	{
		DebugPrint("Copying domain_name to credential");
		_config->credential.domain = wstrDomainname;
	}

	if (!wstrPassword.empty())
	{
		DebugPrint("Copying password to credential");
		_config->credential.password = wstrPassword;
	}


	// Copy the field descriptors for each field. This is useful if you want to vary the 
	// field descriptors based on what Usage scenario the credential was created for.
	// Initialize the fields	

	// !!!!!!!!!!!!!!!!!!!!
	// !!!!!!!!!!!!!!!!!!!!
	// TODO: make _rgCredProvFieldDescriptors dynamically allocated depending on current CPUS
	// !!!!!!!!!!!!!!!!!!!!
	// !!!!!!!!!!!!!!!!!!!!

	//for (DWORD i = 0; SUCCEEDED(hr) && i < ARRAYSIZE(_rgCredProvFieldDescriptors); i++)
	for (DWORD i = 0; SUCCEEDED(hr) && i < Utilities::CredentialFieldCountFor(_config->provider.cpu); i++)
	{
		//DebugPrintLn("Copy field #:");
		//DebugPrintLn(i + 1);
		_rgFieldStatePairs[i] = rgfsp[i];
		hr = FieldDescriptorCopy(rgcpfd[i], &_rgCredProvFieldDescriptors[i]);

		if (FAILED(hr))
			break;

		if (s_rgCredProvFieldInitializorsFor[_config->provider.cpu] != NULL)
			_util.InitializeField(_rgFieldStrings, s_rgCredProvFieldInitializorsFor[_config->provider.cpu][i], i);
	}

	DebugPrint("Init result:");
	if (SUCCEEDED(hr))
		DebugPrint("OK");
	else
		DebugPrint("FAIL");

	return hr;
}

// LogonUI calls this in order to give us a callback in case we need to notify it of anything.
HRESULT CCredential::Advise(
	__in ICredentialProviderCredentialEvents* pcpce
)
{
	//DebugPrintLn(__FUNCTION__);

	if (_pCredProvCredentialEvents != NULL)
	{
		_pCredProvCredentialEvents->Release();
	}
	_pCredProvCredentialEvents = pcpce;
	_pCredProvCredentialEvents->AddRef();

	return S_OK;
}

// LogonUI calls this to tell us to release the callback.
HRESULT CCredential::UnAdvise()
{
	//DebugPrintLn(__FUNCTION__);

	if (_pCredProvCredentialEvents)
	{
		_pCredProvCredentialEvents->Release();
	}
	_pCredProvCredentialEvents = NULL;
	return S_OK;
}

// Callback for the DialogBox
INT_PTR CALLBACK CCredential::ChangePasswordProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{

	wchar_t lpszPassword_old[64];
	wchar_t lpszPassword_new[64];
	wchar_t lpszPassword_new_rep[64];
	WORD cchPassword_new, cchPassword_new_rep, cchPassword_old;

	switch (message)
	{
	case WM_INITDIALOG: {
		DebugPrint("Init change password dialog - START");

		// Get the bitmap to display on top of the dialog (same as the logo of the normal tile)
		static HBITMAP hbmp;
		// Check if custom bitmap was set and load that
		std::string szBitmapPath = PrivacyIDEA::ws2s(_config->bitmapPath);
		if (!szBitmapPath.empty())
		{
			DWORD dwAttrib = GetFileAttributesA(szBitmapPath.c_str());
			if (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY))
			{
				hbmp = (HBITMAP)LoadImageA(NULL, szBitmapPath.c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);
				if (hbmp == NULL)
				{
					DebugPrint("Loading custom tile image for dialog failed:");
					DebugPrint(GetLastError());
				}
			}
		}
		else {
			// Load the default otherwise
			hbmp = LoadBitmap(HINST_THISDLL, MAKEINTRESOURCE(IDB_TILE_IMAGE));
		}
		// Send the bitmap to the picture control
		SendDlgItemMessage(hDlg, IDC_PICTURE, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hbmp);

		// Languagecode for German(Germany) is 1031
		if (GetUserDefaultUILanguage() == 1031) {
			// Set hints for inputs
			SendDlgItemMessage(hDlg, IDC_EDIT_USERNAME, EM_SETCUEBANNER, (WPARAM)TRUE, (LPARAM)L"Benutzer");
			SendDlgItemMessage(hDlg, IDC_EDIT_OLD_PW, EM_SETCUEBANNER, (WPARAM)TRUE, (LPARAM)L"altes Passwort");
			SendDlgItemMessage(hDlg, IDC_EDIT_NEW_PW, EM_SETCUEBANNER, (WPARAM)TRUE, (LPARAM)L"neues Passwort");
			SendDlgItemMessage(hDlg, IDC_EDIT_NEW_PW_REP, EM_SETCUEBANNER, (WPARAM)TRUE, (LPARAM)L"neues Passwort wiederholen");
			SetWindowText(hDlg, L"Passwort �ndern");
		}
		else {
			// Set hints for inputs
			SendDlgItemMessage(hDlg, IDC_EDIT_USERNAME, EM_SETCUEBANNER, (WPARAM)TRUE, (LPARAM)L"Username");
			SendDlgItemMessage(hDlg, IDC_EDIT_OLD_PW, EM_SETCUEBANNER, (WPARAM)TRUE, (LPARAM)L"Old Password");
			SendDlgItemMessage(hDlg, IDC_EDIT_NEW_PW, EM_SETCUEBANNER, (WPARAM)TRUE, (LPARAM)L"New Password");
			SendDlgItemMessage(hDlg, IDC_EDIT_NEW_PW_REP, EM_SETCUEBANNER, (WPARAM)TRUE, (LPARAM)L"Retype New Password");
			SetWindowText(hDlg, L"Change Password");
		}
		// Set focus to old password edit
		PostMessage(hDlg, WM_SETFOCUS, 0, 0);

		// concat domain\user and put it in the edit control
		std::wstring domainWithUser = _config->credential.username + L"\\" + _config->credential.domain;
		//std::wstring tmp_user = std::wstring(Data::Gui::Get()->user_name);
		//std::wstring tmp_domain = std::wstring(Data::Gui::Get()->domain_name);
		//domainWithUser.append(tmp_domain).append(L"\\").append(tmp_user);
		SetDlgItemText(hDlg, IDC_EDIT_USERNAME, domainWithUser.c_str());
		//SetDlgItemText(hDlg, IDC_EDIT_OLD_PW, Data::Gui::Get()->ldap_pass);

		// Set password character to " * "
		SendDlgItemMessage(hDlg, IDC_EDIT_OLD_PW, EM_SETPASSWORDCHAR, (WPARAM)'*', (LPARAM)0);
		SendDlgItemMessage(hDlg, IDC_EDIT_NEW_PW, EM_SETPASSWORDCHAR, (WPARAM)'*', (LPARAM)0);
		SendDlgItemMessage(hDlg, IDC_EDIT_NEW_PW_REP, EM_SETPASSWORDCHAR, (WPARAM)'*', (LPARAM)0);

		// Set the default push button to "Cancel." 
		SendMessage(hDlg, DM_SETDEFID, (WPARAM)IDCANCEL, (LPARAM)0);

		// Center the window
		RECT rc, rcDlg, rcOwner;
		HWND hwndOwner;
		hwndOwner = GetDesktopWindow();

		GetWindowRect(hwndOwner, &rcOwner);
		GetWindowRect(hDlg, &rcDlg);
		CopyRect(&rc, &rcOwner);

		// Offset the owner and dialog box rectangles so that right and bottom values represent the width and height
		// then offset the owner again to discard space taken up by the dialog box. 
		OffsetRect(&rcDlg, -rcDlg.left, -rcDlg.top);
		OffsetRect(&rc, -rc.left, -rc.top);
		OffsetRect(&rc, -rcDlg.right, -rcDlg.bottom);

		// The new position is the sum of half the remaining space and the owner's original position. 
		SetWindowPos(hDlg,
			HWND_TOP,
			rcOwner.left + (rc.right / 2),
			rcOwner.top + (rc.bottom / 2),
			0, 0,          // Ignores size arguments. 
			SWP_NOSIZE);
		DebugPrint("Init change password dialog - END");
		return TRUE;
	}
	case WM_COMMAND: {
		// Set the default push button to "OK" when the user enters text. 
		if (HIWORD(wParam) == EN_CHANGE &&
			LOWORD(wParam) == IDC_EDIT_NEW_PW)
		{
			SendMessage(hDlg, DM_SETDEFID, (WPARAM)IDOK, (LPARAM)0);
		}
		switch (wParam)
		{
		case IDOK: {	// User pressed OK - evaluate the inputs
			DebugPrint("Evaluate change password dialog - START");
			// Get number of characters for each input
			cchPassword_old = (WORD)SendDlgItemMessage(hDlg, IDC_EDIT_OLD_PW, EM_LINELENGTH, (WPARAM)0, (LPARAM)0);
			cchPassword_new = (WORD)SendDlgItemMessage(hDlg, IDC_EDIT_NEW_PW, EM_LINELENGTH, (WPARAM)0, (LPARAM)0);
			cchPassword_new_rep = (WORD)SendDlgItemMessage(hDlg, IDC_EDIT_NEW_PW_REP, EM_LINELENGTH, (WPARAM)0, (LPARAM)0);

			if (cchPassword_new >= 64 || cchPassword_new_rep >= 64 || cchPassword_old >= 64)
			{
				if (GetUserDefaultUILanguage() == 1031) {
					MessageBox(hDlg, L"Passwort zu lang.", L"Fehler", MB_OK);
				}
				else {
					MessageBox(hDlg, L"Password too long.", L"Error", MB_OK);
				}
				return FALSE;
			}
			else if (cchPassword_new == 0 || cchPassword_new_rep == 0 || cchPassword_old == 0)
			{
				if (GetUserDefaultUILanguage() == 1031) {
					MessageBox(hDlg, L"Bitte f�llen Sie alle Felder aus.", L"Fehler", MB_OK);
				}
				else {
					MessageBox(hDlg, L"Please fill the form entirely.", L"Error", MB_OK);
				}
				return FALSE;
			}

			// Put the number of characters into first word of buffer.
			*((LPWORD)lpszPassword_old) = cchPassword_old;
			*((LPWORD)lpszPassword_new) = cchPassword_new;
			*((LPWORD)lpszPassword_new_rep) = cchPassword_new_rep;

			// Get the characters from line 0 (wparam) into buffer lparam
			SendDlgItemMessage(hDlg, IDC_EDIT_OLD_PW, EM_GETLINE, (WPARAM)0, (LPARAM)lpszPassword_old);
			SendDlgItemMessage(hDlg, IDC_EDIT_NEW_PW, EM_GETLINE, (WPARAM)0, (LPARAM)lpszPassword_new);
			SendDlgItemMessage(hDlg, IDC_EDIT_NEW_PW_REP, EM_GETLINE, (WPARAM)0, (LPARAM)lpszPassword_new_rep);

			// Null-terminate each string. 
			lpszPassword_old[cchPassword_old] = 0;
			lpszPassword_new[cchPassword_new] = 0;
			lpszPassword_new_rep[cchPassword_new_rep] = 0;

			// Compare new passwords
			if (wcscmp(lpszPassword_new, lpszPassword_new_rep) != 0) {
				if (GetUserDefaultUILanguage() == 1031) {
					MessageBox(hDlg, L"Neue Passw�rter stimmen nicht �berein!", L"Fehler", MB_OK);
				}
				else {
					MessageBox(hDlg, L"New Passwords do not match!", L"Error", MB_OK);
				}
				return FALSE;
			}
			// copy new password to password for auto-login
			_config->credential.password = wstring(lpszPassword_new);

			/*if (Data::Gui::Get()->ldap_pass || lpszPassword_old)
				wcscpy_s(Data::Gui::Get()->ldap_pass, lpszPassword_old);
			copyNewVals(lpszPassword_new); */

			PWSTR user = const_cast<PWSTR>(_config->credential.username.c_str());
			PWSTR domain = const_cast<PWSTR>(_config->credential.domain.c_str());

			// pcpgsr and pcpcs are set in GetSerialization
			HRESULT	hr = _util.KerberosChangePassword(_config->provider.pcpgsr,
				_config->provider.pcpcs,
				user, lpszPassword_old, lpszPassword_new, domain);

			if (SUCCEEDED(hr))
			{
				_config->credential.passwordMustChange = false;
				_config->credential.passwordChanged = true;
				_config->clearFields = false;
			}
			else
			{
				// TODO
			}

			_config->bypassPrivacyIDEA = true;

			DebugPrint("Evaluate CHANGE PASSWORD DIALOG - END");
			EndDialog(hDlg, TRUE);
			return TRUE;
		}
		case IDCANCEL: {
			// Dialog canceled, reset everything
			DebugPrint("Exit change password dialog - CANCELED");

			_config->credential.passwordMustChange = false;
			_config->credential.passwordChanged = false;
			_config->bypassPrivacyIDEA = false;
			DebugPrint("Exit change password dialog - Data::General RESET ");
			EndDialog(hDlg, TRUE);
			_config->provider.pCredentialProviderEvents->CredentialsChanged(_config->provider.upAdviseContext);
			return TRUE;
		}
		}
		return 0;
	}
	}
	return FALSE;
	UNREFERENCED_PARAMETER(lParam);
}

// LogonUI calls this function when our tile is selected (zoomed).
// If you simply want fields to show/hide based on the selected state,
// there's no need to do anything here - you can set that up in the 
// field definitions.  But if you want to do something
// more complicated, like change the contents of a field when the tile is
// selected, you would do it here.
HRESULT CCredential::SetSelected(__out BOOL* pbAutoLogon)
{
	DebugPrint(__FUNCTION__);
	*pbAutoLogon = false;
	HRESULT hr = S_OK;

	if (_config->pushAuthenticationSuccessful)
	{
		*pbAutoLogon = true;
	}

	if (_config->credential.passwordMustChange && _config->provider.cpu == CPUS_UNLOCK_WORKSTATION
		&& _config->winVerMajor != 10)
	{
		// We cant handle a password change while the maschine is locked, so we guide the user to sign out and in again like windows does
		DebugPrint("Password must change in CPUS_UNLOCK_WORKSTATION");
		_pCredProvCredentialEvents->SetFieldString(this, LUFI_OTP_LARGE_TEXT, L"Go back until you are asked to sign in.");
		_pCredProvCredentialEvents->SetFieldString(this, LUFI_OTP_SMALL_TEXT, L"To change your password sign out and in again.");
		_pCredProvCredentialEvents->SetFieldState(this, LUFI_OTP_LDAP_PASS, CPFS_HIDDEN);
		_pCredProvCredentialEvents->SetFieldState(this, LUFI_OTP_PASS, CPFS_HIDDEN);
	}

	// if passwordMustChange, we want to skip this to get the dialog spawned in GetSerialization
	// if passwordChanged, we want to auto-login
	if (_config->credential.passwordMustChange || _config->credential.passwordChanged)
	{
		if (_config->provider.cpu == CPUS_LOGON || _config->winVerMajor == 10)
		{
			*pbAutoLogon = true;
			DebugPrint("Password change mode LOGON - AutoLogon true");
		}
		else
		{
			DebugPrint("Password change mode UNLOCK - AutoLogon false");
		}
	}

	return hr;
}

// Similarly to SetSelected, LogonUI calls this when your tile was selected
// and now no longer is. The most common thing to do here (which we do below)
// is to clear out the password field.
HRESULT CCredential::SetDeselected()
{
	DebugPrint(__FUNCTION__);

	HRESULT hr = S_OK;

	_util.Clear(_rgFieldStrings, _rgCredProvFieldDescriptors, this, _pCredProvCredentialEvents, CLEAR_FIELDS_EDIT_AND_CRYPT);

	//// CONCRETE
	_util.ResetScenario(this, _pCredProvCredentialEvents);
	////

	// Reset password changing in case another user wants to log in
	if (_config->credential.passwordChanged)
	{
		_config->credential.passwordChanged = false;
	}
	// If its not UNLOCK_WORKSTATION we keep this status to keep the info to sign out first
	if (_config->credential.passwordMustChange && (_config->provider.cpu != CPUS_UNLOCK_WORKSTATION))
	{
		_config->credential.passwordMustChange = false;
	}

	return hr;
}

// Gets info for a particular field of a tile. Called by logonUI to get information to 
// display the tile.
HRESULT CCredential::GetFieldState(
	__in DWORD dwFieldID,
	__out CREDENTIAL_PROVIDER_FIELD_STATE* pcpfs,
	__out CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE* pcpfis
)
{
	//DebugPrintLn(__FUNCTION__);

	HRESULT hr;

	// Validate paramters.
	if ((dwFieldID < Utilities::CredentialFieldCountFor(_config->provider.cpu)) && pcpfs && pcpfis)
	{
		*pcpfs = _rgFieldStatePairs[dwFieldID].cpfs;
		*pcpfis = _rgFieldStatePairs[dwFieldID].cpfis;

		hr = S_OK;
	}
	else
	{
		hr = E_INVALIDARG;
	}

	//DebugPrintLn(hr);

	return hr;
}

// Sets ppwsz to the string value of the field at the index dwFieldID.
HRESULT CCredential::GetStringValue(
	__in DWORD dwFieldID,
	__deref_out PWSTR* ppwsz
)
{
	//DebugPrintLn(__FUNCTION__);

	HRESULT hr;

	// Check to make sure dwFieldID is a legitimate index.
	if (dwFieldID < Utilities::CredentialFieldCountFor(_config->provider.cpu) && ppwsz)
	{
		// Make a copy of the string and return that. The caller
		// is responsible for freeing it.
		hr = SHStrDupW(_rgFieldStrings[dwFieldID], ppwsz);
	}
	else
	{
		hr = E_INVALIDARG;
	}

	//DebugPrintLn(hr);

	return hr;
}

// Gets the image to show in the user tile.
HRESULT CCredential::GetBitmapValue(
	__in DWORD dwFieldID,
	__out HBITMAP* phbmp
)
{
	DebugPrint(__FUNCTION__);

	HRESULT hr;
	if ((LUFI_OTP_LOGO == dwFieldID) && phbmp)
	{
		HBITMAP hbmp = NULL;
		LPCSTR lpszBitmapPath = PrivacyIDEA::ws2s(_config->bitmapPath).c_str();
		DebugPrint(lpszBitmapPath);

		if (NOT_EMPTY(lpszBitmapPath))
		{
			DWORD dwAttrib = GetFileAttributesA(lpszBitmapPath);

			DebugPrint(dwAttrib);

			if (dwAttrib != INVALID_FILE_ATTRIBUTES
				&& !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY))
			{
				hbmp = (HBITMAP)LoadImageA(NULL, lpszBitmapPath, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);

				if (hbmp == NULL)
				{
					DebugPrint(GetLastError());
				}
			}
		}

		if (hbmp == NULL)
		{
			hbmp = LoadBitmap(HINST_THISDLL, MAKEINTRESOURCE(IDB_TILE_IMAGE));
		}

		if (hbmp != NULL)
		{
			hr = S_OK;
			*phbmp = hbmp;
		}
		else
		{
			hr = HRESULT_FROM_WIN32(GetLastError());
		}
	}
	else
	{
		hr = E_INVALIDARG;
	}

	DebugPrint(hr);

	return hr;
}

// Sets pdwAdjacentTo to the index of the field the submit button should be 
// adjacent to. We recommend that the submit button is placed next to the last
// field which the user is required to enter information in. Optional fields
// should be below the submit button.
HRESULT CCredential::GetSubmitButtonValue(
	__in DWORD dwFieldID,
	__out DWORD* pdwAdjacentTo
)
{
	DebugPrint("ID:" + to_string(dwFieldID));
	HRESULT hr;

	// Validate parameters.

	// !!!!!!!!!!!!!
	// !!!!!!!!!!!!!
	// TODO: Change scenario data structures to determine correct submit-button and pdwAdjacentTo dynamically
	// pdwAdjacentTo is a pointer to the fieldID you want the submit button to appear next to.

	if (LPFI_OTP_SUBMIT_BUTTON == dwFieldID && pdwAdjacentTo)
	{
		if (_config->isSecondStep)
		{
			*pdwAdjacentTo = LPFI_OTP_PASS;
		}
		else
		{
			*pdwAdjacentTo = LPFI_OTP_LDAP_PASS;
		}
		hr = S_OK;
	}/*
	else if (CPFI_OTP_SUBMIT_BUTTON == dwFieldID && pdwAdjacentTo)
	{
		*pdwAdjacentTo = CPFI_OTP_PASS_NEW_2;
		hr = S_OK;
	} */
	else
	{
		hr = E_INVALIDARG;
	}

	return hr;
}

// Sets the value of a field which can accept a string as a value.
// This is called on each keystroke when a user types into an edit field.
HRESULT CCredential::SetStringValue(
	__in DWORD dwFieldID,
	__in PCWSTR pwz
)
{
	HRESULT hr;

	// Validate parameters.
	if (dwFieldID < Utilities::CredentialFieldCountFor(_config->provider.cpu) &&
		(CPFT_EDIT_TEXT == _rgCredProvFieldDescriptors[dwFieldID].cpft ||
			CPFT_PASSWORD_TEXT == _rgCredProvFieldDescriptors[dwFieldID].cpft))
	{
		PWSTR* ppwszStored = &_rgFieldStrings[dwFieldID];
		CoTaskMemFree(*ppwszStored);
		hr = SHStrDupW(pwz, ppwszStored);
	}
	else
	{
		hr = E_INVALIDARG;
	}

	//DebugPrintLn(hr);

	return hr;
}

// Returns the number of items to be included in the combobox (pcItems), as well as the 
// currently selected item (pdwSelectedItem).
HRESULT CCredential::GetComboBoxValueCount(
	__in DWORD dwFieldID,
	__out DWORD* pcItems,
	__out_range(< , *pcItems) DWORD* pdwSelectedItem
)
{
	DebugPrint(__FUNCTION__);

	// Validate parameters.
	if (dwFieldID < Utilities::CredentialFieldCountFor(_config->provider.cpu) &&
		(CPFT_COMBOBOX == _rgCredProvFieldDescriptors[dwFieldID].cpft))
	{
		// UNUSED
		*pcItems = 0;
		*pdwSelectedItem = 0;
		return S_OK;
	}
	else
	{
		return E_INVALIDARG;
	}
}

// Called iteratively to fill the combobox with the string (ppwszItem) at index dwItem.
HRESULT CCredential::GetComboBoxValueAt(
	__in DWORD dwFieldID,
	__in DWORD dwItem,
	__deref_out PWSTR* ppwszItem)
{
	DebugPrint(__FUNCTION__);
	UNREFERENCED_PARAMETER(dwItem);
	UNREFERENCED_PARAMETER(dwFieldID);
	UNREFERENCED_PARAMETER(ppwszItem);

	return E_INVALIDARG;
	/*
	// Validate parameters.
	if (dwFieldID < Utilities::CredentialFieldCountFor(_config->provider.cpu) &&
		(CPFT_COMBOBOX == _rgCredProvFieldDescriptors[dwFieldID].cpft))
	{
		// UNUSED
		return E_INVALIDARG;
	}
	else
	{
		return E_INVALIDARG;
	}
	*/
}

// Called when the user changes the selected item in the combobox.
HRESULT CCredential::SetComboBoxSelectedValue(
	__in DWORD dwFieldID,
	__in DWORD dwSelectedItem
)
{
	DebugPrint(__FUNCTION__);
	UNREFERENCED_PARAMETER(dwSelectedItem);
	// Validate parameters.
	if (dwFieldID < Utilities::CredentialFieldCountFor(_config->provider.cpu) &&
		(CPFT_COMBOBOX == _rgCredProvFieldDescriptors[dwFieldID].cpft))
	{
		return S_OK;
	}
	else
	{
		return E_INVALIDARG;
	}
}

HRESULT CCredential::GetCheckboxValue(
	__in DWORD dwFieldID,
	__out BOOL* pbChecked,
	__deref_out PWSTR* ppwszLabel
)
{
	// Called to check the initial state of the checkbox
	DebugPrint(__FUNCTION__);
	UNREFERENCED_PARAMETER(dwFieldID);
	UNREFERENCED_PARAMETER(ppwszLabel);
	*pbChecked = FALSE;
	//SHStrDupW(L"Use offline token.", ppwszLabel); // TODO custom text?

	return S_OK;
}

HRESULT CCredential::SetCheckboxValue(
	__in DWORD dwFieldID,
	__in BOOL bChecked
)
{
	UNREFERENCED_PARAMETER(dwFieldID);
	UNREFERENCED_PARAMETER(bChecked);
	DebugPrint(__FUNCTION__);
	return S_OK;
}

//------------- 
// The following methods are for logonUI to get the values of various UI elements and then communicate
// to the credential about what the user did in that field.  However, these methods are not implemented
// because our tile doesn't contain these types of UI elements
HRESULT CCredential::CommandLinkClicked(__in DWORD dwFieldID)
{
	UNREFERENCED_PARAMETER(dwFieldID);
	DebugPrint(__FUNCTION__);
	return S_OK;
}

//------ end of methods for controls we don't have in our tile ----//

// Collect the username and password into a serialized credential for the correct usage scenario 
// (logon/unlock is what's demonstrated in this sample).  LogonUI then passes these credentials 
// back to the system to log on.
HRESULT CCredential::GetSerialization(
	__out CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE* pcpgsr,
	__out CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs,
	__deref_out_opt PWSTR* ppwszOptionalStatusText,
	__out CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon
)
{
	DebugPrint(__FUNCTION__);

	*pcpgsr = CPGSR_RETURN_NO_CREDENTIAL_FINISHED;

	HRESULT hr = E_FAIL, retVal = S_OK;



	/*
	CPGSR_NO_CREDENTIAL_NOT_FINISHED
	No credential was serialized because more information is needed.

	CPGSR_NO_CREDENTIAL_FINISHED
	This serialization response means that the Credential Provider has not serialized a credential but it has completed its work. This response has multiple meanings.
	It can mean that no credential was serialized and the user should not try again. This response can also mean no credential was submitted but the credential�s work is complete.
	For instance, in the Change Password scenario, this response implies success.

	CPGSR_RETURN_CREDENTIAL_FINISHED
	A credential was serialized. This response implies a serialization structure was passed back.

	CPGSR_RETURN_NO_CREDENTIAL_FINISHED
	The credential provider has not serialized a credential, but has completed its work. The difference between this value and CPGSR_NO_CREDENTIAL_FINISHED is that this flag
	will force the logon UI to return, which will unadvise all the credential providers.
	*/

	_config->provider.pCredProvCredentialEvents = _pCredProvCredentialEvents;
	_config->provider.pCredProvCredential = this;

	_config->provider.pcpcs = pcpcs;
	_config->provider.pcpgsr = pcpgsr;

	_config->provider.status_icon = pcpsiOptionalStatusIcon;
	_config->provider.status_text = ppwszOptionalStatusText;

	_config->provider.field_strings = _rgFieldStrings;
	_config->provider.num_field_strings = Utilities::CredentialFieldCountFor(_config->provider.cpu);

	// TODO THIS IS VERY BAD
	PWSTR currentUsername = const_cast<PWSTR>(_config->credential.username.c_str());
	PWSTR currentPassword = const_cast<PWSTR>(_config->credential.password.c_str());
	PWSTR currentDomain = const_cast<PWSTR>(_config->credential.domain.c_str());

	// open dialog for old/new password
	if (_config->credential.passwordMustChange)
	{
		// TODO rebuild password change
		/*
		HWND hwndOwner = nullptr;
		HRESULT res = E_FAIL;
		if (_pCredProvCredentialEvents)
		{
			res = _pCredProvCredentialEvents->OnCreatingWindow(&hwndOwner); // get a handle to the owner window
		}
		if (SUCCEEDED(res))
		{
			if (_pConfiguration->provider.usage_scenario == CPUS_LOGON || _pConfiguration->winVerMajor == 10)
			{//It's password change on Logon we can handle that
				DebugPrintLn("Passwordchange with CPUS_LOGON - open Dialog");
				::DialogBox(HINST_THISDLL,					// application instance
					MAKEINTRESOURCE(IDD_DIALOG1),			// dialog box resource
					hwndOwner,								// owner window
					ChangePasswordProc						// dialog box window procedure
				);
				//goto CleanUpAndReturn;
			}
		}
		else
		{
			DbgRelPrintLn("Opening password change dialog failed: Handle to owner window is missing");
		} */
	}

	if (_config->credential.passwordChanged)
	{
		DebugPrint("Password change success- Set Data::General for autologon");
		_config->bypassPrivacyIDEA = true;
	}


	if (_config->userCanceled)
	{
		*_config->provider.status_icon = CPSI_ERROR;
		*_config->provider.pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
		SHStrDupW(L"Logon cancelled", _config->provider.status_text);
		return S_FALSE;
	}
	// Check if we are pre 2nd step
	if (_config->authenticationSuccessful == false && _config->pushAuthenticationSuccessful == false)
	{
		// Prepare for the second step (input only OTP)
		_config->clearFields = false;
		_util.SetScenario(_config->provider.pCredProvCredential,
			_config->provider.pCredProvCredentialEvents,
			SCENARIO::SECOND_STEP, std::wstring(), L"Please enter your second factor:");
		*_config->provider.pcpgsr = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
	}
	// It is the final step -> try to log in
	else if (_config->authenticationSuccessful || _config->pushAuthenticationSuccessful || _config->bypassPrivacyIDEA)
	{
		if (_piStatus == PI_AUTH_SUCCESS)
		{
			// If windows password is wrong, treat it as new logon
			_config->isSecondStep = false;
			_piStatus = PI_STATUS_NOT_SET;
		}

		if (_config->provider.cpu == CPUS_CREDUI)
		{
			hr = _util.CredPackAuthentication(pcpgsr, pcpcs, _config->provider.cpu,
				currentUsername,
				currentPassword, currentDomain);
		}
		else
		{
			hr = _util.KerberosLogon(pcpgsr, pcpcs, _config->provider.cpu,
				currentUsername,
				currentPassword, currentDomain);
		}
		if (SUCCEEDED(hr))
		{
			if (_config->credential.passwordChanged)
				_config->credential.passwordChanged = false;
		}
		else
		{
			retVal = S_FALSE;
		}
	}
	else if (_piStatus == PI_AUTH_FAILURE) // TODO
	{
		wstring otpFailureText = _config->otpFailureText.empty() ? L"Wrong One-Time-Password!" : _config->otpFailureText;
		SHStrDupW(otpFailureText.c_str(), _config->provider.status_text);
		*_config->provider.status_icon = CPSI_ERROR;
		*_config->provider.pcpgsr = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
	}
	else
	{
		wstring message = L"Unexpected Error";
		/*
		switch (_endpointStatus)
		{
			case ENDPOINT_STATUS_AUTH_FAIL: message = L"Wrong otp";
		} */// TODO

		// Display the error message and icon
		SHStrDupW(message.c_str(), _config->provider.status_text);
		*_config->provider.status_icon = CPSI_ERROR;

		// Jump to the first login window
		_util.ResetScenario(this, _pCredProvCredentialEvents);
		retVal = S_FALSE;
	}

	if (_config->clearFields)
	{
		_util.Clear(_rgFieldStrings, _rgCredProvFieldDescriptors, this, _pCredProvCredentialEvents, CLEAR_FIELDS_CRYPT);
	}
	else
	{
		_config->clearFields = true; // it's a one-timer...
	}

	// Reset privacyIDEA status 
	_piStatus = PI_STATUS_NOT_SET;

#ifdef _DEBUG
	if (pcpgsr)
	{
		if (*pcpgsr == CPGSR_NO_CREDENTIAL_FINISHED) { DebugPrint("CPGSR_NO_CREDENTIAL_FINISHED"); }
		if (*pcpgsr == CPGSR_NO_CREDENTIAL_NOT_FINISHED) { DebugPrint("CPGSR_NO_CREDENTIAL_NOT_FINISHED"); }
		if (*pcpgsr == CPGSR_RETURN_CREDENTIAL_FINISHED) { DebugPrint("CPGSR_RETURN_CREDENTIAL_FINISHED"); }
		if (*pcpgsr == CPGSR_RETURN_NO_CREDENTIAL_FINISHED) { DebugPrint("CPGSR_RETURN_NO_CREDENTIAL_FINISHED"); }
	}
	else { DebugPrint("pcpgsr is a nullpointer!"); }
	DebugPrint("CCredential::GetSerialization - END");
#endif //_DEBUG
	return retVal;
}

// If push is successful, reset the credential to set autologin in 
void CCredential::pushAuthenticationCallback(bool success)
{
	DebugPrint(__FUNCTION__);
	if (success)
	{
		_config->pushAuthenticationSuccessful = true;
		// When autologon is triggered, connect is called instantly, therefore bypass privacyIDEA on next run
		_config->bypassPrivacyIDEA = true;
		_config->provider.pCredentialProviderEvents->CredentialsChanged(_config->provider.upAdviseContext);
	}
}

// Connect is called first after the submit button is pressed.
HRESULT CCredential::Connect(__in IQueryContinueWithStatus* pqcws)
{
	DebugPrint(__FUNCTION__);

	_config->pQueryContinueWithStatus = pqcws;

	_config->provider.pCredProvCredential = this;
	_config->provider.pCredProvCredentialEvents = _pCredProvCredentialEvents;
	_config->provider.field_strings = _rgFieldStrings;
	_config->provider.num_field_strings = Utilities::CredentialFieldCountFor(_config->provider.cpu);
	_util.ReadFieldValues();

	if (_config->bypassPrivacyIDEA == false)
	{
		if (_config->twoStepHideOTP && !_config->isSecondStep)
		{
			if (!_config->twoStepSendEmptyPassword && !_config->twoStepSendPassword)
			{
				// Delay for a short moment, otherwise logonui crashes (???)
				this_thread::sleep_for(chrono::milliseconds(200));
				// Then skip to next step
			}
			else if (_config->twoStepSendEmptyPassword && !_config->twoStepSendPassword)
			{
				// Send an empty pass in the FIRST step
				_piStatus = _privacyIDEA.validateCheck(_config->credential.username, _config->credential.domain, L"");
				if (_piStatus == PI_TRIGGERED_CHALLENGE)
				{
					// TODO do same with triggered challenge?
				}
			}
			else if (!_config->twoStepSendEmptyPassword && _config->twoStepSendPassword)
			{
				// Send the windows password in the first step, which may trigger challenges
				_piStatus = _privacyIDEA.validateCheck(_config->credential.username, _config->credential.domain,
					_config->credential.password);
				if (_piStatus == PI_TRIGGERED_CHALLENGE)
				{
					Challenge c = _privacyIDEA.getCurrentChallenge();
					_config->challenge = c;
					if (!c.transaction_id.empty())
					{
						// Set a message in any case
						wstring msg = c.getAggregatedMessage();
						if (!msg.empty())
							pqcws->SetStatusMessage(msg.c_str());
						else
							pqcws->SetStatusMessage(_config->defaultChallengeText.c_str());

						// if both pushtoken and classic OTP are available, start polling in background
						if (c.tta == TTA::BOTH)
						{
							// When polling finishes, pushauthenticationsuccess has to be set, then resetscenario has to be called
							_privacyIDEA.asyncPollTransaction(PrivacyIDEA::ws2s(_config->credential.username), c.transaction_id,
								std::bind(&CCredential::pushAuthenticationCallback, this, std::placeholders::_1));
						}
						// if only push is available, start polling in main thread 
						else if (c.tta == TTA::PUSH)
						{
							while (_piStatus != PI_TRANSACTION_SUCCESS)
							{
								_piStatus = _privacyIDEA.pollTransaction(c.transaction_id);

								this_thread::sleep_for(300ms);

								if (pqcws->QueryContinue() != S_OK) //TODO cancel button??
								{
									_config->userCanceled = true;
									break;
								}
							}

							_piStatus = (_piStatus == PI_TRANSACTION_SUCCESS) ? PI_AUTH_SUCCESS : PI_AUTH_FAILURE;
							if (_piStatus == PI_AUTH_SUCCESS) _config->authenticationSuccessful = true;
						}
					}
					else
					{
						DebugPrint("Triggering challenge returned no transaction_id");
					}
				}
				else
				{
					// Only classic OTP available, do nothing else in the first step
				}
			}
			else
			{
				DebugPrint("UNKNOWN STATE IN CCREDENTIAL::CONNECT");
			}
			_config->isSecondStep = true;
		}
		//////////////////// SECOND STEP ////////////////////////
		else if (_config->twoStepHideOTP && _config->isSecondStep)
		{
			// Send with optional transaction_id from first step
			_piStatus = _privacyIDEA.validateCheck(
				PrivacyIDEA::ws2s(_config->credential.username),
				PrivacyIDEA::ws2s(_config->credential.domain),
				PrivacyIDEA::ws2s(_config->credential.otp),
				_config->challenge.transaction_id);
			if (_piStatus == PI_OFFLINE_OTP_SUCCESS || _piStatus == PI_AUTH_SUCCESS)
			{
				_config->authenticationSuccessful = true;
			}
		}
		//////// NORMAL SETUP WITH 3 FIELDS -> SEND OTP ////////
		else
		{
			_piStatus = _privacyIDEA.validateCheck(_config->credential.username,
				_config->credential.domain, _config->credential.otp);
			if (_piStatus == PI_OFFLINE_OTP_SUCCESS || _piStatus == PI_AUTH_SUCCESS)
			{
				_config->authenticationSuccessful = true;
			}
		}
	}
	else
		DebugPrint("bypassing privacyIDEA...");

	_config->pQueryContinueWithStatus = nullptr;
	DebugPrint("Connect - END");
	return S_OK; // always S_OK
}

HRESULT CCredential::Disconnect()
{
	return E_NOTIMPL;
}

// ReportResult is completely optional.  Its purpose is to allow a credential to customize the string
// and the icon displayed in the case of a logon failure.  For example, we have chosen to 
// customize the error shown in the case of bad username/password and in the case of the account
// being disabled.
HRESULT CCredential::ReportResult(
	__in NTSTATUS ntsStatus,
	__in NTSTATUS ntsSubstatus,
	__deref_out_opt PWSTR* ppwszOptionalStatusText,
	__out CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon
)
{
#ifdef _DEBUG
	DebugPrint(__FUNCTION__);
	// only print interesting statuses
	if (ntsStatus != 0)
	{
		DebugPrint("ntsStatus:");
		DebugPrint(ntsStatus);
	}
	if (ntsSubstatus != 0)
	{
		DebugPrint("ntsSubstatus:");
		DebugPrint(ntsSubstatus);
	}
#endif

	UNREFERENCED_PARAMETER(ppwszOptionalStatusText);
	UNREFERENCED_PARAMETER(pcpsiOptionalStatusIcon);

	_config->credential.passwordMustChange = (ntsStatus == STATUS_PASSWORD_MUST_CHANGE) || (ntsSubstatus == STATUS_PASSWORD_EXPIRED);

	if (_config->credential.passwordMustChange)
	{
		DebugPrint("Status: Password must change");
		return E_NOTIMPL;
	}

	// check if the password update was NOT successfull
	// these two are for new passwords not conform to password policies
	bool pwNotUpdated = (ntsStatus == STATUS_PASSWORD_RESTRICTION) || (ntsSubstatus == STATUS_ILL_FORMED_PASSWORD);
	if (pwNotUpdated)
	{
		DebugPrint("Status: Password update failed: Not conform to policies");
	}
	// this catches the wrong old password, 
	pwNotUpdated = pwNotUpdated || ((ntsStatus == STATUS_LOGON_FAILURE) && (ntsSubstatus == STATUS_INTERNAL_ERROR));

	if (pwNotUpdated)
	{
		// it wasn't updated so we start over again
		_config->credential.passwordMustChange = true;
		_config->credential.passwordChanged = false;
	}

	if (ntsStatus == STATUS_LOGON_FAILURE && !pwNotUpdated)
	{
		_util.ResetScenario(this, _pCredProvCredentialEvents);
	}
	return E_NOTIMPL;
}
