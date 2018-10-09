#include "process.hpp"
#include "ntdll.hpp"
#include "logger.hpp"
#include "transformer.hpp"
#include <Psapi.h>
#include <algorithm>

process::operator bool()
{
	return static_cast<bool>(this->handle);
}

process process::current_process()
{
	return process(reinterpret_cast<HANDLE>(-1));
}

uint32_t process::from_name(const std::string& process_name)
{
	DWORD process_list[516], bytes_needed;
	if (EnumProcesses(process_list, sizeof(process_list), &bytes_needed))
	{
		for (size_t index = 0; index < bytes_needed / sizeof(uint32_t); index++)
		{
			auto proc = process(process_list[index], PROCESS_ALL_ACCESS);

			if (!proc)
				continue;

			if (process_name == proc.get_name())
				return process_list[index];
		}
	}

	return 0;
}

MEMORY_BASIC_INFORMATION process::virtual_query(const uintptr_t address)
{
	MEMORY_BASIC_INFORMATION mbi;

	VirtualQueryEx(this->handle.get_handle(), reinterpret_cast<LPCVOID>(address), &mbi, sizeof(MEMORY_BASIC_INFORMATION));

	return mbi;
}

uintptr_t process::raw_allocate(const SIZE_T virtual_size, const uintptr_t address)
{
	return reinterpret_cast<uintptr_t>(
		VirtualAllocEx(this->handle.get_handle(), reinterpret_cast<LPVOID>(address), virtual_size, MEM_COMMIT, PAGE_EXECUTE_READWRITE)
		);
}

bool process::free_memory(const uintptr_t address)
{
	return VirtualFreeEx(this->handle.get_handle(), reinterpret_cast<LPVOID>(address), NULL, MEM_RELEASE);
}


bool process::read_raw_memory(const void* buffer, const uintptr_t address, const SIZE_T size)
{
	return ReadProcessMemory(
		this->handle.get_handle(),
		reinterpret_cast<LPCVOID>(address),
		const_cast<void*>(buffer),
		size,
		nullptr);
}

bool process::write_raw_memory(const void* buffer, const SIZE_T size, const uintptr_t address)
{
	return WriteProcessMemory(
		this->handle.get_handle(),
		reinterpret_cast<LPVOID>(address),
		buffer,
		size,
		nullptr);
}

bool process::virtual_protect(const uintptr_t address, uint32_t protect, uint32_t* old_protect)
{
	return VirtualProtectEx(
		this->handle.get_handle(),
		reinterpret_cast<LPVOID>(address),
		0x1000,
		protect,
		reinterpret_cast<PDWORD>(old_protect));
}

uintptr_t process::map(memory_section& section)
{
	void* base_address = nullptr;
	SIZE_T view_size = section.size;

	auto result = ntdll::NtMapViewOfSection(
		section.handle.get_handle(),
		this->handle.get_handle(),
		&base_address,
		NULL, NULL, NULL,
		&view_size,
		2, 0,
		section.protection);

	if (!NT_SUCCESS(result))
	{
		logger::log_error("NtMapViewOfSection failed");
		logger::log_formatted("Error code", result, true);
	}

	return reinterpret_cast<uintptr_t>(base_address);
}

std::unordered_map<std::string, uintptr_t> process::get_modules()
{
	auto result = std::unordered_map<std::string, uintptr_t>();

	HMODULE module_handles[1024];
	DWORD size_needed;

	if (!EnumProcessModules(this->handle.get_handle(), module_handles, sizeof(module_handles), &size_needed))
		return result;

	for (auto module_index = 0; module_index < size_needed / sizeof(HMODULE); module_index++)
	{
		// INITIALISE STRING OF SIZE MAX_PATH (260)
		std::string module_name(MAX_PATH, '0');

		// GET MODULE NAME
		GetModuleBaseNameA(
			this->handle.get_handle(),
			module_handles[module_index],
			const_cast<char*>(module_name.c_str()),
			module_name.size());

		// MAKE CHARACTERS LOWERCASE
		transformer::to_lower(module_name);

		// TRUNCATE SIZE TO NULL TERMINATOR
		transformer::truncate(module_name);

		// SET ENTRY
		result[module_name] = reinterpret_cast<uintptr_t>(module_handles[module_index]);
	}

	return result;
}

std::string process::get_name()
{
	char buffer[MAX_PATH];
	GetModuleBaseNameA(handle.get_handle(), nullptr, buffer, MAX_PATH);

	return std::string(buffer);
}

// thx blackbone :)
uintptr_t process::get_module_export(uintptr_t module_handle, const char* function_ordinal)
{
	IMAGE_DOS_HEADER dos_header;
	IMAGE_NT_HEADERS64 nt_header;
	this->read_memory(&dos_header, module_handle);
	this->read_memory(&nt_header, module_handle + dos_header.e_lfanew);

	auto export_base = nt_header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
	auto export_base_size = nt_header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
	if (!export_base) // CONTAINS EXPORTED FUNCTIONS
	{
		logger::log_error("Module does not contain exports!");
		return 0x00;
	}

	std::unique_ptr<IMAGE_EXPORT_DIRECTORY> export_data_raw(reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(malloc(export_base_size)));
	auto export_data = export_data_raw.get();

	// READ EXPORTED DATA FROM TARGET PROCESS FOR LATER PROCESSING
	if (!this->read_raw_memory(export_data, module_handle + export_base, export_base_size))
		logger::log_error("failed to read export data");

	// BLACKBONE PASTE, NEVER EXPERIENCED THIS BUT WHO KNOWS?
	if (export_base_size <= sizeof(IMAGE_EXPORT_DIRECTORY))
	{
		export_base_size = static_cast<DWORD>(export_data->AddressOfNameOrdinals - export_base
			+ max(export_data->NumberOfFunctions, export_data->NumberOfNames) * 255);

		export_data_raw.reset(reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(malloc(export_base_size)));
		export_data = export_data_raw.get();

		if (!this->read_raw_memory(export_data, module_handle + export_base, export_base_size))
			logger::log_error("failed to read export data");
	}

	// GET DATA FROM READ MEMORY
	auto delta = reinterpret_cast<uintptr_t>(export_data) - export_base;
	auto address_of_ordinals = reinterpret_cast<WORD*>(export_data->AddressOfNameOrdinals + delta);
	auto address_of_names = reinterpret_cast<DWORD*>(export_data->AddressOfNames + delta);
	auto address_of_functions = reinterpret_cast<DWORD*>(export_data->AddressOfFunctions + delta);

	// NO EXPORTED FUNCTIONS? DID WE FUCK UP?
	if (export_data->NumberOfFunctions <= 0)
		logger::log_error("No exports found!");

	auto ptr_function_ordinal = reinterpret_cast<uintptr_t>(function_ordinal);

	// ITERATE EXPORTED FUNCTIONS
	for (size_t export_index = 0; export_index < export_data->NumberOfFunctions; export_index++)
	{
		WORD ordinal;
		std::string function_name;
		auto is_import_by_ordinal = ptr_function_ordinal <= 0xFFFF;

		// GET EXPORT INFORMATION
		ordinal = static_cast<WORD>(is_import_by_ordinal ? export_index : address_of_ordinals[export_index]);
		function_name = reinterpret_cast<char*>(address_of_names[export_index] + delta);

		// IS IT THE FUNCTION WE ASKED FOR?
		auto found_via_ordinal =
			is_import_by_ordinal &&
			static_cast<WORD>(ptr_function_ordinal) == (ordinal + export_data->Base);
		auto found_via_name =
			!is_import_by_ordinal && function_name == function_ordinal;

		if (!found_via_ordinal && !found_via_name)
		{
			continue;
			//logger::log_error("Export not found!");
			//return 0;
		}

		// FORWARDED EXPORT?
		// IF FUNCTION POINTER IS INSIDE THE EXPORT DIRECTORY, IT IS *NOT* A FUNCTION POINTER!
		// FUCKING SHIT MSVCP140 
		// FUCK YOU
		const auto function_pointer = module_handle + address_of_functions[ordinal];
		const auto directory_start = module_handle + export_base;
		const auto directory_end = module_handle + export_base + export_base_size;
		if (function_pointer >= directory_start && function_pointer <= directory_end)
		{
			// READ STRING
			std::uint8_t forwarded_name[255] = { 0 };
			this->read_raw_memory(forwarded_name, function_pointer, sizeof(forwarded_name));

			// PARSE STRING
			std::string forward(reinterpret_cast<char*>(forwarded_name));
			std::string library_name = forward.substr(0, forward.find(".")) + ".dll";
			function_name = forward.substr(forward.find(".") + 1, function_name.npos);

			// LOWERCASE THANKS
			transformer::to_lower(library_name);

			// FIND FORWARDED MODULE
			auto modules = this->get_modules();
			auto search = modules.find(library_name);
			if (search != modules.end())
			{
				// RECURSIVELY HANDLE FORWARDED MODULE :)
				return this->get_module_export(search->second, function_name.c_str());
			}

			// TODO: HANDLE THIS? WHO CARES
			logger::log_error("Forwarded module not loaded"); 
		}

		return function_pointer;
	}

	logger::log_error("Export not found!");
	return 0;
}

HANDLE process::create_thread(const uintptr_t address, const uintptr_t argument)
{
	return CreateRemoteThread(this->handle.get_handle(), nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(address), reinterpret_cast<LPVOID>(argument), 0, nullptr);
}
