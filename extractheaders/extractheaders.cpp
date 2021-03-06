#include "extractheaders.h"

#include "vsparsing.h"
#include <boost/wave/language_support.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/wave.hpp>
#include <boost/wave/cpplexer/cpp_lex_token.hpp>
#include <boost/wave/cpplexer/cpp_lex_iterator.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <string>
#include <queue>
#include <set>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <functional>
#include <direct.h>
#include <regex>
using namespace std;
using namespace boost;
using namespace wave;
using namespace boost::wave;

using namespace boost::filesystem;

typedef boost::wave::cpplexer::lex_token<> token_type;

typedef boost::wave::cpplexer::lex_iterator<token_type> lexer_type;



template <typename Token>
class find_includes_hooks
	: public boost::wave::context_policies::eat_whitespace<Token>
{
public:
	template <typename Context>
	void
		opened_include_file(Context const& ctx, std::string relname,
		std::string absname, bool is_system_include)
	{
		path file_path(relname);
		string filename = strtolower(string(file_path.filename().string()));

		if (impl->input.verbose) {
			impl->output.infoStream << "opened_include_file: " << relname << ", " << absname << "," << is_system_include << std::endl;
		}

		if (find(impl->input.includeheaders.begin(), impl->input.includeheaders.end(), relname) != impl->input.includeheaders.end()) {
			impl->systemheaders.insert(relname);
		}
		else {
			if (find(impl->input.excludeheaders.begin(), impl->input.excludeheaders.end(), filename) == impl->input.excludeheaders.end()) {
				if (impl->include_all() || is_system_include) {
					if (!subpath(impl->input.excludedirs, absname))
						impl->systemheaders.insert(relname);
				}
				else {
					if (!is_system_include)
						impl->userheadersqueue.push(boost::filesystem::canonical(absname));
				}

			}
		}
	}

	template <typename Context>
	void returning_from_include_file(Context const& ctx)
	{

	}

	template <typename Context>
	void
		detected_include_guard(Context const& ctx, std::string const& filename,
		std::string const& include_guard)
	{
	}

	template <typename Context, typename Token>
	void detected_pragma_once(Context const& ctx,
		Token const& pragma_token,
		std::string const& filename)
	{

	}

	template <typename Context>
	bool found_include_directive(Context const& ctx,
		std::string const &filename, bool include_next)
	{
		if (impl->output.headersfound_num.find(filename) == impl->output.headersfound_num.end())
			impl->output.headersfound.push_back(filename);
		++(impl->output.headersfound_num[filename]);
		if (impl->input.verbose) {
			impl->output.infoStream << "found_include_directive: " << filename << "," << include_next << std::endl;
		}
		return false;
	}

	ExtractHeadersImpl* impl = NULL;
};

typedef boost::wave::context<
	std::string::const_iterator, lexer_type,
	boost::wave::iteration_context_policies::load_file_to_string,
	find_includes_hooks<token_type> > context_type;

// splits a semicolon-separated list of words and returns a vector
void splitInput(vector<string>& elems, const string& inputstr)
{
	string elem;

	if (!inputstr.empty()) {
		for (auto character : inputstr) {
			if (character == ';') {
				elems.push_back(elem);
				elem.clear();
			}
			else
				elem += character;
		}

		elems.push_back(elem);
	}
}

class ExtractHeadersImpl {
public:
	ExtractHeadersImpl(ExtractHeadersConsoleOutput& out, const ExtractHeadersInput& in);
	void process_file(const path& filename);
	void add_macro_definitions(context_type& context, const std::string& cxx_flags);
	void add_system_includes(context_type& ctx);
	void add_user_includes(context_type& ctx, const boost::filesystem::path& filename);
	void write_stdafx();
	void run();
	bool include_all()
	{
		return find(input.includeheaders.begin(), input.includeheaders.end(), "*") != input.includeheaders.end();
	}

	queue<path> userheadersqueue;
	set<path> headersprocessed;
	set<path> includedirs;
	set<string> sysincludedirs;
	// system or thirdparty headers
	set<boost::filesystem::path> systemheaders;

	const ExtractHeadersInput& originalInput;
	// same as originalInput but some parameters may contain more information E.g.
	// input.inputs may get some elements from the .vcxproj
	ExtractHeadersInput input;
	ExtractHeadersConsoleOutput& output;
};


ExtractHeaders::~ExtractHeaders()
{
}

int getcommonsize(path filepath, path projectdir)
{
	int commonsize = 0;
	auto itfile = filepath.rbegin();
	auto itprojdir = projectdir.rbegin();
	for (; itfile != filepath.rend() && itprojdir != projectdir.rend();) {
		if (*itfile != *itprojdir) {
			if (commonsize > 0) {
				break;
			}
			else {
				++itfile;
			}
		}
		else {
			++itfile;
			++itprojdir;
			++commonsize;
		}
	}
	return commonsize;
}

bool iscplusplusfile(path filepath)
{
	const char* extensions[] =
	{ ".cpp",
	".cxx",
	".c",
	".cc",
	NULL
	};
	const string file_ext = filepath.extension().string();
	unsigned int pos = 0;

	while (extensions[pos]) {
		if (file_ext == extensions[pos])
			return true;

		pos++;
	}

	return false;
}

path getsrcdir(const vector<string>& files, path projectdir)
{
	path src_dir;
	int curcommonsize = 0;
	for (auto file : files) {
		auto filepath = canonical(path(file).remove_filename());
		if (iscplusplusfile(file)) {
			int commonsize = getcommonsize(filepath, projectdir);
			if (commonsize > curcommonsize) {
				src_dir = filepath;
				curcommonsize = commonsize;
			}
		}
	}
	return src_dir;
}

// gets all files in a dir (non-recursively)
vector<string> getAllFilesInDir(const char* dir)
{
	path path(dir);
	directory_iterator end_iter;
	vector<string> result;

	if (exists(path) && is_directory(path))
	{
		for (directory_iterator dir_iter(path); dir_iter != end_iter; ++dir_iter)
		{
			if (is_regular_file(dir_iter->status()) &&
				iscplusplusfile(dir_iter->path()))
				result.push_back(dir_iter->path().string());
		}
	}

	return result;
}

// gets all dirs in a dir (non-recursively)
vector<string> getAllDirsInDir(const char* dir)
{
	path path(dir);
	directory_iterator end_iter;
	vector<string> result;

	if (exists(path) && is_directory(path))
	{
		for (directory_iterator dir_iter(path); dir_iter != end_iter; ++dir_iter)
		{
			if (is_directory(dir_iter->status()))
				result.push_back(dir_iter->path().string());
		}
	}

	return result;
}

void make_absolute(string& oldpath, const path& dir)
{

	path newpath(oldpath.c_str());

	if (!newpath.is_absolute()) {
		try {
			newpath = boost::filesystem::absolute(newpath, dir);
			oldpath = newpath.string();
		}
		catch (std::exception& e) {
			// TODO ghc log this error somehow?
		}
	}
}


bool subpath(const vector<string> paths, const string& path)
{
	vector<string>::const_iterator path_it = paths.begin();
	boost::filesystem::path canonical_path = boost::filesystem::canonical(path);

	while (path_it != paths.end()) {
		boost::filesystem::path exclude_path = boost::filesystem::canonical(*path_it);

		if (canonical_path.string().find(exclude_path.string()) != string::npos)
			return true;

		path_it++;
	}

	return false;
}

string trim(string& str)
{
	size_t first = str.find_first_not_of(' ');
	size_t last = str.find_last_not_of(' ');

	return str.substr(first, (last - first + 1));
}



ExtractHeaders::ExtractHeaders()

{
}

ExtractHeadersImpl::ExtractHeadersImpl(ExtractHeadersConsoleOutput& out, const ExtractHeadersInput& in) :
    originalInput(in),
	input(in),
	output(out)
{
}

void ExtractHeaders::write_stdafx()
{
	assert(impl && "run has not been called yet");
	impl->write_stdafx();
}

bool ExtractHeaders::run(ExtractHeadersConsoleOutput& output, const ExtractHeadersInput& input)
{
	impl.reset(new ExtractHeadersImpl(output, input));
	try {
		impl->run();
		return true;
	}
	catch (std::exception& e) {
		output.errorStream << e.what() << endl;
		return false;
	}
}

void ExtractHeadersImpl::add_macro_definitions(context_type& context, const string& cxx_flags)
{
	std::vector<std::string> definitions;
	splitInput(definitions, cxx_flags);
	for (auto definition : definitions) {
		context.add_macro_definition(definition, false);
		if (input.verbose)
			output.infoStream << "add_macro_definition: " << definition << std::endl;
	}
}


void ExtractHeadersImpl::add_system_includes(context_type& ctx)
{

	for (auto& sysdir : input.sysincludetreedirs) {

		sysincludedirs.insert(sysdir);

		if (is_directory(sysdir)) {
			vector<string> dirs = getAllDirsInDir(sysdir.c_str());

			for (auto& dir : dirs) {

				sysincludedirs.insert(dir);
			}
		}
	}

	for (auto dir : sysincludedirs)  {
		ctx.add_sysinclude_path(dir.c_str());
		if (input.verbose)
			output.infoStream << "add_sysinclude_path: " << dir << std::endl;
	}
}

void ExtractHeadersImpl::add_user_includes(context_type& ctx, const path& filename)
{
	path filepath(filename);

	for (auto& userdir : input.includetreedirs) {

		includedirs.insert(boost::filesystem::canonical(userdir));

		if (is_directory(userdir)) {
			vector<string> dirs = getAllDirsInDir(userdir.c_str());

			for (auto& dir : dirs) {

				includedirs.insert(boost::filesystem::canonical(dir));
			}
		}
	}

	includedirs.insert(filepath.remove_filename().string().c_str());

	for (auto dir : includedirs)  {
		ctx.add_include_path(dir.string().c_str());
		if (input.verbose)
			output.infoStream << "add_include_path: " << dir << std::endl;
	}
}

void ExtractHeadersImpl::process_file(const path& filename)
{
	//  create the wave::context object and initialize it from the file to
	//  preprocess (may contain options inside of special comments)

	std::ifstream instream(filename.string().c_str());
	string instr;
	context_type::iterator_type it;
	context_type::iterator_type end;
	bool is_end = false;
	string cxxflagsstr;
	find_includes_hooks <token_type> hooks;


	hooks.impl = this;
	instream.unsetf(std::ios::skipws);
	instr = std::string(std::istreambuf_iterator<char>(instream.rdbuf()),
		std::istreambuf_iterator<char>());

	if (input.verbose)
		output.infoStream << "Preprocessing input file: " << filename.generic_string()
		<< "..." << std::endl;

	context_type ctx(instr.begin(),
		instr.end(),
		filename.string().c_str(),
		hooks);

	ctx.set_language(language_support(support_cpp | support_cpp11 |
		support_option_variadics |
		support_option_long_long |
		support_option_include_guard_detection
		));
	// it is best not to go too deep, headers like <iostream> on windows
	// give problems with boost wave
	ctx.set_max_include_nesting_depth(input.nesting);

	for (auto def : input.cxxflags) {

		if (def.back() == ';')
			def.resize(def.size() - 1);

		if (cxxflagsstr.empty())
			cxxflagsstr += def;
		else
			cxxflagsstr += ";" + def;
	}

	add_macro_definitions(ctx, cxxflagsstr);

	for (const auto& includeDir : input.includedirsIn)
	{
		if (boost::filesystem::exists(includeDir)) {
			includedirs.insert(boost::filesystem::canonical(includeDir));
		}
	}

	for (const auto& includeDir : input.sysincludedirs)
	{
		sysincludedirs.insert(includeDir);
	}

	input.excludeheaders.push_back("stdafx.h");
	input.excludeheaders.push_back("stdafx.cpp");
	input.excludeheaders.push_back(input.outputfile);
    add_system_includes(ctx);
    add_user_includes(ctx, filename);

	//  preprocess the input, loop over all generated tokens collecting the
	//  generated text
	it = ctx.begin();
	end = ctx.end();

	// perform actual preprocessing
	do
	{
		using namespace boost::wave;

		try {
			++it;
			// operator != could also throw an exception
			is_end = (it == end);
		}
		catch (boost::wave::cpplexer::lexing_exception const& e) {

			std::string filename = e.file_name();
			output.errorStream
				<< filename << "(" << e.line_no() << "): "
				<< "Lexical error: " << e.description() << std::endl;
			break;
		}
		catch (boost::wave::cpp_exception const& e) {
			if (e.get_errorcode() != preprocess_exception::include_nesting_too_deep) {
				std::string filename = e.file_name();
				output.errorStream
					<< filename << "(" << e.line_no() << "): "
					<< e.description() << std::endl;
			}
		}
	} while (!is_end);
}

void ExtractHeadersImpl::write_stdafx()
{
	if (systemheaders.empty()) {
		output.infoStream << "No system header found." << endl;
		return;
	}
	path outputpath(input.outputfile);
	string guardname = outputpath.filename().string();
	size_t dotpos = guardname.find_first_of(".");
	std::ofstream outputStream;

    outputStream = std::ofstream(input.outputfile);
	guardname = guardname.substr(0, dotpos);

	path outputSrcPath = outputpath.remove_filename() / path(guardname + ".cpp");
	if (!exists(outputSrcPath)) {
		std::ofstream outputSrcStream = std::ofstream(outputSrcPath.string());

		if (!outputSrcStream.is_open()) {
			cerr << "Cannot open: " << outputSrcPath.string();
			exit(EXIT_FAILURE);
		}

		outputSrcStream << "#include \"" << guardname << ".h\"\n";
	}

	for (auto & c : guardname)
		c = toupper(c);

	if (!outputStream.is_open()) {
		cerr << "Cannot open: " << input.outputfile;
		exit(EXIT_FAILURE);
	}

	outputStream << "/* Machine generated code */\n\n";

	if (input.pragma)
		outputStream << "#pragma once\n\n";
	else {
		outputStream << "#ifndef " + guardname + "_H\n";
		outputStream << "#define " + guardname + "_H\n";
	}

	if (input.mostincluded > 0) {
		std::vector<pair<string, int>> vt(output.headersfound_num.begin(), output.headersfound_num.end());
		sort(vt.begin(), vt.end(), [](const pair<string, int>& lhs, const pair<string, int>& rhs) -> bool {return lhs.second > rhs.second; });
		for (auto header_num : vt) {
			output.infoStream << header_num.first << ": " << header_num.second << endl;
			if (header_num.second >= input.mostincluded) {
				outputStream << "#include " << header_num.first << "\n";
			}
		}
	}
	else {
		for (auto& header : output.headersfound) {
			string trimmed_headername = trim(header);
			string unquoted = trimmed_headername.substr(1, trimmed_headername.length() - 2);
			auto header_it = systemheaders.begin();
			while (header_it != systemheaders.end()) {
				string headername = header_it->filename().string();
				if (headername.find_last_of(unquoted) != std::string::npos) {
					if (!include_all() && trimmed_headername.size() >= 2 &&
						(trimmed_headername)[0] == '"' &&
						trimmed_headername[trimmed_headername.size() - 1] == '"') {
						if (find(input.includeheaders.begin(), input.includeheaders.end(), unquoted) == input.includeheaders.end())
							break;
					}

					outputStream << "#include " << header << "\n";
					systemheaders.erase(header_it);
					break;
				}
				header_it++;
			}
		}
	}


	if (!input.pragma)
		outputStream << "#endif\n";

	output.infoStream << "Precompiled header generated at: " << canonical(input.outputfile).string() << endl;
}


// @throws runtime_error if an input path does no exist
void ExtractHeadersImpl::run()
{
	path src_dir;
	if (!input.vcproj.empty()) {
		try {
			VcprojParsing parser(input.vcproj.c_str(), output.errorStream);
			vector<ProjectConfiguration> configurations;
			vector<ProjectConfiguration>::iterator configuration_it;
			vector<string> files;
			string definitions;
			string additionalIncludeDirectories;
			string precompiledHeaderFile;
			string configurationName;
			path vcxproj_dir = canonical(path(input.vcproj).remove_filename());

			parser.parse(configurations, files);
			src_dir = getsrcdir(files, vcxproj_dir);

			if (configurations.empty())
				throw runtime_error("File: " + input.vcproj + " contains no configurations");

			// if the user did not define the configuration to get the macros and include directories from,
			// we just choose the first one
			definitions = configurations[0].definitions;
			additionalIncludeDirectories = configurations[0].additionalIncludeDirectories;
			precompiledHeaderFile = configurations[0].precompiledHeaderFile;
			configuration_it = configurations.begin();

			while (configuration_it != configurations.end()) {

				if (configuration_it->name == input.configuration) {
					definitions = configuration_it->definitions;
					additionalIncludeDirectories = configuration_it->additionalIncludeDirectories;
					precompiledHeaderFile = configuration_it->precompiledHeaderFile;
					configurationName = configuration_it->configuration;
				}

				configuration_it++;
			}

			if (!definitions.empty())
				input.cxxflags.push_back(definitions);

			for (auto file : files) {
				boost::replace_all(file, "$(Configuration)", configurationName);
				make_absolute(file, vcxproj_dir);
				input.inputs.push_back(file);
			}

			if (!additionalIncludeDirectories.empty()) {
				vector<string> directories;

				splitInput(directories, additionalIncludeDirectories);

				for (auto dir : directories) {
					boost::replace_all(dir, "$(Configuration)", configurationName);

					// because we do not which ones are system include
					// directories and which are user include dirs, we
					// add what we find in the vcxproj to both
					make_absolute(dir, vcxproj_dir);
					input.includedirsIn.push_back(dir);
					input.sysincludedirs.push_back(dir);
				}

			}

			if (!precompiledHeaderFile.empty()) {
				make_absolute(precompiledHeaderFile, vcxproj_dir);
				input.outputfile = precompiledHeaderFile;
			}
			else {
				make_absolute(input.outputfile, src_dir.empty() ? vcxproj_dir : src_dir);
			}
		} catch (runtime_error& ex) {
			throw runtime_error(string("Cannot parse: ") + input.vcproj + ": " + ex.what());
		}
	}

	if (exists(input.outputfile) && !input.force) {
		throw runtime_error(string("Precompiled header already exists: ") + input.outputfile);
	}

	for (auto& input : input.inputs) {
		vector<string> inputList;
		splitInput(inputList, input);

		for (auto& input_path : inputList) {

			if (is_directory(input_path)) {
				vector<string> files = getAllFilesInDir(input_path.c_str());

				for (auto& file : files) {
					userheadersqueue.push(boost::filesystem::canonical(file));
				}
			}
			else if (!exists(input_path))
				output.errorStream << "Cannot find: " << input_path << "\n";
			else
				userheadersqueue.push(boost::filesystem::canonical(input_path));
		}
	}

	if (!input.vcproj.empty()) {
		output.infoStream << endl;
		output.infoStream << "********************************************************************************" << endl;
		output.infoStream << "Generating precompiled header for " << input.vcproj << endl;
		output.infoStream << "Src dir:" << src_dir.string() << endl;
	}
	output.infoStream << "Processing " << userheadersqueue.size() << " reported includes" << endl;

	while (!userheadersqueue.empty()) {
		path header = userheadersqueue.front();


		if (headersprocessed.count(header) == 0) {
			auto regexp_it = input.excluderegexp.begin();
			bool match = false;
            regex excluderegexp;

            if (regexp_it != input.excluderegexp.end())
                excluderegexp = regex(*regexp_it);

			while (regexp_it != input.excluderegexp.end() &&
				   !(match = regex_match(header.filename().string(), excluderegexp))) {
				regexp_it++;
			}

			if (!match && find(input.excludeheaders.begin(), input.excludeheaders.end(), header.filename().string()) == input.excludeheaders.end())
			   process_file(header);

			headersprocessed.insert(header);
		}

		userheadersqueue.pop();
	}
}

std::string& strtolower(std::string& str)
{
	transform(str.begin(), str.end(), str.begin(), ::tolower);

	return str;
}
