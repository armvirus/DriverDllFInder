#include <fstream>
#include <iostream>
#include <windows.h>
#include <vector>
#include <filesystem>

inline std::string get_service_path(SC_HANDLE sc_handle, const std::string& service_name)
{
	SC_HANDLE hService(OpenServiceA(sc_handle, service_name.c_str(), SERVICE_QUERY_CONFIG));
	if (!hService)
		return "";

	std::vector<BYTE> buffer;
	DWORD dwBytesNeeded = sizeof(QUERY_SERVICE_CONFIGA);
	LPQUERY_SERVICE_CONFIGA pConfig;

	do
	{
		buffer.resize(dwBytesNeeded);
		pConfig = (LPQUERY_SERVICE_CONFIGA)&buffer[0];

		if (QueryServiceConfigA(hService, pConfig, buffer.size(), &dwBytesNeeded))
			return pConfig->lpBinaryPathName;
	} while (GetLastError() == ERROR_INSUFFICIENT_BUFFER);

	return "";
}

inline std::string get_driver_name(std::string const& path)
{
	return path.substr(path.find_last_of("/\\") + 1);
}

inline std::vector<std::string>get_active_drivers_array()
{
	SC_HANDLE manager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

	if (manager == INVALID_HANDLE_VALUE)
	{
		return {};
	}

	DWORD bytes_needed;
	DWORD service_count;

	BOOL status = EnumServicesStatusExA(
		manager,
		SC_ENUM_PROCESS_INFO,
		SERVICE_DRIVER,
		SERVICE_ACTIVE,
		0,
		0,
		&bytes_needed,
		&service_count,
		0,
		0
	);

	PBYTE bytes_array = (PBYTE)malloc(bytes_needed);

	status = EnumServicesStatusExA(
		manager,
		SC_ENUM_PROCESS_INFO,
		SERVICE_DRIVER,
		SERVICE_ACTIVE,
		bytes_array,
		bytes_needed,
		&bytes_needed,
		&service_count,
		0,
		0
	);

	ENUM_SERVICE_STATUS_PROCESSA* service_array = (ENUM_SERVICE_STATUS_PROCESSA*)bytes_array;

	std::vector<std::string>return_array{};

	for (int i = 0; i < service_count; i++)
	{
		std::string driver_path = get_service_path(manager, service_array[i].lpServiceName);
		std::string driver_name = get_driver_name(driver_path);

		if (!driver_path.empty() && !driver_name.empty())
		{
			return_array.push_back(driver_name);
		}
	}

	free(bytes_array);

	return return_array;
}

inline std::tuple<std::uintptr_t, std::size_t> get_section(std::uintptr_t local_module_base, std::string section_name)
{
	const auto module_dos_header = reinterpret_cast<PIMAGE_DOS_HEADER>(local_module_base);

	if (module_dos_header->e_magic != IMAGE_DOS_SIGNATURE)
	{
		return{};
	}

	const auto module_nt_headers = reinterpret_cast<PIMAGE_NT_HEADERS>(local_module_base + module_dos_header->e_lfanew);

	if (module_nt_headers->Signature != IMAGE_NT_SIGNATURE)
	{
		return{};
	}

	const auto section_count = module_nt_headers->FileHeader.NumberOfSections;
	const auto section_headers = IMAGE_FIRST_SECTION(module_nt_headers);

	for (WORD i = 0; i < section_count; ++i)
	{
		if (strcmp(reinterpret_cast<char*>(section_headers[i].Name), section_name.c_str()) == 0)
		{
			return { section_headers[i].VirtualAddress, section_headers[i].SizeOfRawData };
		}
	}

	return {};
}

int main(int argument_count, char** argument_array)
{
	if (argument_count != 3)
	{
		std::printf("[-] usage: driverdllfinder.exe driver.sys/module.dll .section_name\n");
		return -1;
	}

	if (std::filesystem::exists(argument_array[1]) == false)
	{
		std::printf("[-] file [%s] does not exist\n", argument_array[1]);
		return -1;
	}

	std::ifstream drv_buffer_str(argument_array[1], std::ios::binary);
	std::vector<std::uint8_t>drv_buffer(std::istreambuf_iterator<char>(drv_buffer_str), {});

	auto dos_header = reinterpret_cast<PIMAGE_DOS_HEADER>(drv_buffer.data());
	auto nt_header = reinterpret_cast<PIMAGE_NT_HEADERS>(drv_buffer.data() + dos_header->e_lfanew);

	if (nt_header->Signature != IMAGE_NT_SIGNATURE)
	{
		std::printf("[-] invalid nt signature of target file\n");

		return -1;
	}

	bool dll = strcmp(std::filesystem::path(argument_array[1]).extension().string().c_str(), ".dll") == 0;
	std::uint32_t image_size = nt_header->OptionalHeader.SizeOfImage;

	std::printf("searching for potential [%s] files with section [%s] image size [0x%x]\n", dll == true ? ".dll" : ".sys", argument_array[2], image_size);

	std::vector<std::string>active_drivers = get_active_drivers_array();

	int result_count = 0;

	for (const auto& current_driver : std::filesystem::directory_iterator(dll == true ? "C:\\Windows\\System32" : "C:\\Windows\\System32\\drivers"))
	{
		if (strcmp(current_driver.path().extension().string().c_str(), std::filesystem::path(argument_array[1]).extension().string().c_str()) != 0)
		{
			continue;
		}
		
		if (dll == false)
		{
			bool skip = false;
			for (auto active_driver_it : active_drivers)
			{
				if (!_strcmpi(current_driver.path().filename().string().c_str(), active_driver_it.c_str()))
				{
					skip = true;
				}
			}
			if (skip == true) continue;
		}

		std::ifstream input(current_driver.path().string(), std::ios::binary);
		std::vector<std::uint8_t>file_buffer(std::istreambuf_iterator<char>(input), {});

		auto dos_header = reinterpret_cast<PIMAGE_DOS_HEADER>(file_buffer.data());
		if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) continue;

		auto nt_header = reinterpret_cast<PIMAGE_NT_HEADERS>(file_buffer.data() + dos_header->e_lfanew);
		if (nt_header->Signature != IMAGE_NT_SIGNATURE) continue;

		const auto& [section_offset, section_size] = get_section(reinterpret_cast<std::uintptr_t>(file_buffer.data()), argument_array[2]);

		if (image_size < section_size)
		{
			std::printf("[+] [%s] [%s] > [0x%x]\n", current_driver.path().string().c_str(), argument_array[2], section_size);
			result_count++;
		}
	}

	std::printf("[+] found [%i] potential results\n", result_count);

	return 0;
}
