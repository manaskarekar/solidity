/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>
#include <libsolidity/interface/ReadFile.h>
#include <libsolidity/lsp/LanguageServer.h>

#include <liblangutil/SourceReferenceExtractor.h>

#include <libsolutil/Visitor.h>
#include <libsolutil/JSON.h>

#include <boost/filesystem.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <ostream>

#include <iostream>
#include <string>

using namespace std;
using namespace std::placeholders;

using namespace solidity::langutil;
using namespace solidity::frontend;

namespace solidity::lsp {

namespace // {{{ helpers
{

// TODO: maybe use SimpleASTVisitor here, if that would be a simple free-fuunction :)
class ASTNodeLocator : public ASTConstVisitor
{
private:
	int m_pos = -1;
	ASTNode const* m_currentNode = nullptr;

public:
	explicit ASTNodeLocator(int _pos): m_pos{_pos}
	{
	}

	ASTNode const* closestMatch() const noexcept { return m_currentNode; }

	bool visitNode(ASTNode const& _node) override
	{
		if (_node.location().start <= m_pos && m_pos <= _node.location().end)
		{
			m_currentNode = &_node;
			return true;
		}
		return false;
	}
};

// TODO: maybe use SimpleASTVisitor here, if that would be a simple free-fuunction :)
class ReferenceCollector: public frontend::ASTConstVisitor
{
private:
	frontend::Declaration const& m_declaration;
	std::vector<::lsp::Server::DocumentHighlight> m_result;

public:
	explicit ReferenceCollector(frontend::Declaration const& _declaration):
		m_declaration{_declaration}
	{
		fprintf(stderr, "finding refs: '%s', '%s'\n",
				_declaration.name().c_str(),
				_declaration.location().text().c_str());
	}

	std::vector<::lsp::Server::DocumentHighlight> take() { return std::move(m_result); }

	static std::vector<::lsp::Server::DocumentHighlight> collect(frontend::Declaration const& _declaration, frontend::ASTNode const& _ast)
	{
		auto collector = ReferenceCollector(_declaration);
		_ast.accept(collector);
		return collector.take();
	}

	bool visit(Identifier const& _identifier) override
	{
		if (auto const declaration = _identifier.annotation().referencedDeclaration; declaration)
			if (declaration == &m_declaration)
				addReference(_identifier.location(), "(identifier)");

		return visitNode(_identifier);
	}

	void addReference(SourceLocation const& _location, char const* msg = "")
	{
		auto const [startLine, startColumn] = _location.source->translatePositionToLineColumn(_location.start);
		auto const [endLine, endColumn] = _location.source->translatePositionToLineColumn(_location.end);
		auto const locationRange = ::lsp::Range{
			{startLine, startColumn},
			{endLine, endColumn}
		};

		fprintf(stderr, " -> found reference %s at %d:%d .. %d:%d\n",
			msg,
			startLine, startColumn,
			endLine, endColumn
		);

		auto highlight = ::lsp::Server::DocumentHighlight{};
		highlight.range = locationRange;
		highlight.kind = ::lsp::Server::DocumentHighlightKind::Text; // TODO: are you being read or written to?

		m_result.emplace_back(highlight);
	}

	// TODO: MemberAccess

	bool visitNode(ASTNode const& _node) override
	{
		if (&_node == &m_declaration)
		{
			if (auto const* decl = dynamic_cast<Declaration const*>(&_node))
				addReference(decl->nameLocation(), "(visitNode)");
			else
				addReference(_node.location(), "(visitNode)");
		}

		return true;
	}
};

} // }}} end helpers

LanguageServer::LanguageServer(::lsp::Transport& _client, Logger _logger):
	::lsp::Server(_client, std::move(_logger)),
	m_vfs()
{
}

void LanguageServer::shutdown()
{
	logInfo("LanguageServer: shutdown requested");
}

LanguageServer::InitializeResponse LanguageServer::initialize(
	string _rootUri,
	map<string, string> _settings,
	Trace _trace,
	vector<WorkspaceFolder> _workspaceFolders
)
{
	(void) _trace; // TODO: debuglog based on this config
	(void) _settings; // TODO: user settings (such as EVM version)

#if !defined(NDEBUG)
	ostringstream msg;
	msg << "LanguageServer: rootUri : " << _rootUri << endl;
	for (auto const& workspace: _workspaceFolders)
		msg << "                workspace folder: " << workspace.name << "; " << workspace.uri << endl;
#endif

	if (boost::starts_with(_rootUri, "file:///"))
	{
		auto const fspath = boost::filesystem::path(_rootUri.substr(7));
		m_basePath = fspath;
		m_allowedDirectories.push_back(fspath);
	}

#if !defined(NDEBUG)
	logMessage(msg.str());
#endif

	InitializeResponse hello{};
	hello.serverName = "solc";
	hello.serverVersion = string(solidity::frontend::VersionNumber);
	hello.supportsDefinition = true;
	hello.supportsDocumentHighlight = true;
	hello.supportsDocumentSync = true;
	hello.supportsReferences = true;
	hello.supportsHover = false; // TODO
	return hello;
}

void LanguageServer::initialized()
{
	// NB: this means the client has finished initializing. Now we could maybe start sending
	// events to the client.
	logMessage("LanguageServer: Client initialized");
}

void LanguageServer::documentOpened(string const& _uri, string _languageId, int _documentVersion, std::string _contents)
{
	logMessage("LanguageServer: Opening document: " + _uri);

	::lsp::vfs::File const& file = m_vfs.insert(
		_uri,
		_languageId,
		_documentVersion,
		_contents
	);

	validate(file);
}

void LanguageServer::documentContentUpdated(string const& _uri, optional<int> _version, vector<DocumentChange> _changes)
{
	// TODO: all this info is actually unrelated to solidity/lsp specifically except knowing that
	// the file has updated, so we can  abstract that away and only do the re-validation here.
	if (::lsp::vfs::File* file = m_vfs.find(_uri); file != nullptr)
	{
		if (_version.has_value())
			file->setVersion(_version.value());

		for (DocumentChange const& change: _changes)
		{
#if !defined(NDEBUG)
			ostringstream str;
			str << "did change: " << change.range << " for '" << change.text << "'";
			logMessage(str.str());
#endif
			file->modify(change.range, change.text);
		}

		validate(*file);
	}
	else
		logError("LanguageServer: File to be modified not opened \"" + _uri + "\"");
}

void LanguageServer::documentContentUpdated(string const& _uri, optional<int> _version, string const& _fullContentChange)
{
	if (::lsp::vfs::File* file = m_vfs.find(_uri); file != nullptr)
	{
		if (_version.has_value())
			file->setVersion(_version.value());

		file->replace(_fullContentChange);

		validate(*file);
	}
	else
		logError("LanguageServer: File to be modified not opened \"" + _uri + "\"");
}

void LanguageServer::documentClosed(string const& _uri)
{
	logMessage("LanguageServer: didClose: " + _uri);
}

void LanguageServer::validateAll()
{
	for (reference_wrapper<::lsp::vfs::File const> const& file: m_vfs.files())
		validate(file.get());
}

void LanguageServer::validate(::lsp::vfs::File const& _file)
{
	vector<PublishDiagnostics> result;
	validate(_file, result);

	for (auto const& diag: result)
		pushDiagnostics(diag);
}

frontend::ReadCallback::Result LanguageServer::readFile(string const& _kind, string const& _path)
{
	return m_fileReader->readFile(_kind, _path);
}

constexpr ::lsp::Server::DiagnosticSeverity toDiagnosticSeverity(Error::Type _errorType)
{
	switch (_errorType)
	{
		case Error::Type::CodeGenerationError:
		case Error::Type::DeclarationError:
		case Error::Type::DocstringParsingError:
		case Error::Type::ParserError:
		case Error::Type::SyntaxError:
		case Error::Type::TypeError:
			return ::lsp::Server::DiagnosticSeverity::Error;
		case Error::Type::Warning:
			return ::lsp::Server::DiagnosticSeverity::Warning;
	}
	// Should never be reached.
	return ::lsp::Server::DiagnosticSeverity::Error;
}

void LanguageServer::compile(::lsp::vfs::File const& _file)
{
	// TODO: optimize! do not recompile if nothing has changed (file(s) not flagged dirty).

	// always start fresh when compiling
	m_sourceCodes.clear();

	m_sourceCodes[_file.uri().substr(7)] = _file.contentString();

	m_fileReader = make_unique<FileReader>(m_basePath, m_allowedDirectories);

	m_compilerStack.reset();
	m_compilerStack = make_unique<CompilerStack>(bind(&FileReader::readFile, ref(*m_fileReader), _1, _2));

	// TODO: configure all compiler flags like in CommandLineInterface (TODO: refactor to share logic!)
	OptimiserSettings settings = OptimiserSettings::standard(); // TODO: get from config
	m_compilerStack->setOptimiserSettings(settings);
	m_compilerStack->setParserErrorRecovery(false);
	m_compilerStack->setEVMVersion(EVMVersion::constantinople()); // TODO: get from config
	m_compilerStack->setRevertStringBehaviour(RevertStrings::Default); // TODO get from config
	m_compilerStack->setSources(m_sourceCodes);

	m_compilerStack->compile();
}

void LanguageServer::validate(::lsp::vfs::File const& _file, vector<PublishDiagnostics>& _result)
{
	compile(_file);

	PublishDiagnostics params{};
	params.uri = _file.uri();

	for (shared_ptr<Error const> const& error: m_compilerStack->errors())
	{
		// Don't show this warning. "This is a pre-release compiler version."
		if (error->errorId().error == 3805)
			continue;

		auto const message = SourceReferenceExtractor::extract(*error);

		auto const severity = toDiagnosticSeverity(error->type());

		// global warnings don't have positions in the source code - TODO: default them to top of file?

		auto const startPosition = LineColumn{{
			max(message.primary.position.line, 0),
			max(message.primary.startColumn, 0)
		}};

		auto const endPosition = LineColumn{{
			max(message.primary.position.line, 0),
			max(message.primary.endColumn, 0)
		}};

		Diagnostic diag{};
		diag.range.start.line = startPosition.line;
		diag.range.start.column = startPosition.column;
		diag.range.end.line = endPosition.line;
		diag.range.end.column = endPosition.column;
		diag.message = message.primary.message;
		diag.source = "solc";
		diag.severity = severity;

		for (SourceReference const& secondary: message.secondary)
		{
			auto related = DiagnosticRelatedInformation{};

			related.message = secondary.message;
			related.location.uri = "file://" + secondary.sourceName; // is the sourceName always a fully qualified path?
			related.location.range.start.line = secondary.position.line;
			related.location.range.start.column = secondary.startColumn;
			related.location.range.end.line = secondary.position.line; // what about multiline?
			related.location.range.end.column = secondary.endColumn;

			diag.relatedInformation.emplace_back(move(related));
		}

		if (message.errorId.has_value())
			diag.code = message.errorId.value().error;

		params.diagnostics.emplace_back(move(diag));
	}

	// Personally, I think they should stay, because it is nice to get reports on these.
	// We could make these diagnostics optional or even part of solc compiler itself.
	// (Currently this only checks the whole file, but it should instead just look at comments)
#if 1
	for (size_t pos = _file.contentString().find("FIXME", 0); pos != string::npos; pos = _file.contentString().find("FIXME", pos + 1))
	{
		Diagnostic diag{};
		diag.message = "Hello, FIXME's should be fixed.";
		diag.range.start = _file.buffer().toPosition(pos);
		diag.range.end = {diag.range.start.line, diag.range.start.column + 5};
		diag.severity = DiagnosticSeverity::Error;
		diag.source = "solc";
		params.diagnostics.emplace_back(diag);
	}

	for (size_t pos = _file.contentString().find("TODO", 0); pos != string::npos; pos = _file.contentString().find("FIXME", pos + 1))
	{
		Diagnostic diag{};
		diag.message = "Please remember to create a ticket on GitHub for that.";
		diag.range.start = _file.buffer().toPosition(pos);
		diag.range.end = {diag.range.start.line, diag.range.start.column + 5};
		diag.severity = DiagnosticSeverity::Hint;
		diag.source = "solc";
		params.diagnostics.emplace_back(diag);
	}
#endif

	_result.emplace_back(params);
}

frontend::ASTNode const* LanguageServer::findASTNode(::lsp::Position const& _position, std::string const& _fileName)
{
	if (!m_compilerStack)
		return nullptr;

	frontend::ASTNode const& sourceUnit = m_compilerStack->ast(_fileName);
	auto const sourcePos = sourceUnit.location().source->translateLineColumnToPosition(_position.line + 1, _position.column + 1);

	ASTNodeLocator m{sourcePos};
	sourceUnit.accept(m);
	auto const closestMatch = m.closestMatch();
	if (closestMatch != nullptr)
		fprintf(stderr, "findASTNode for %s @ pos=%d:%d (%d), symbol: '%s' (%s)\n",
				sourceUnit.location().source->name().c_str(),
				sourcePos,
				_position.line,
				_position.column,
				closestMatch->location().text().c_str(), typeid(*closestMatch).name());
	else
		fprintf(stderr, "findASTNode for pos=%d:%d (%d), not found.\n",
				sourcePos,
				_position.line,
				_position.column);

	return closestMatch;
}

optional<LanguageServer::Location> LanguageServer::gotoDefinition(DocumentPosition _location)
{
	if (auto const file = m_vfs.find(_location.uri); file != nullptr)
	{
		compile(*file);
		solAssert(m_compilerStack.get() != nullptr, "");

		auto const sourceName = file->uri().substr(7); // strip "file://"

		if (auto const sourceNode = findASTNode(_location.position, sourceName); sourceNode)
		{
			if (auto const importDirective = dynamic_cast<ImportDirective const*>(sourceNode))
			{
				// When cursor is on an import directive, then we want to jump to the actual file that
				// is being imported.
				if (auto const fpm = m_fileReader->fullPathMapping().find(importDirective->path()); fpm != m_fileReader->fullPathMapping().end())
				{
					Location output{};
					output.uri = "file://" + fpm->second;
					return {output};
				}
				else
					return nullopt; // definition not found
			}
			else if (auto const n = dynamic_cast<frontend::MemberAccess const*>(sourceNode); n)
			{
				// For scope members, jump to the naming symbol of the referencing declaration of this member.
				auto const declaration = n->annotation().referencedDeclaration;

				if (auto const loc = declarationPosition(declaration); loc.has_value())
				{
					auto const sourceName = declaration->location().source->name();
					auto const fullSourceName = m_fileReader->fullPathMapping().at(sourceName);
					Location output{};
					output.range = loc.value();
					output.uri = "file://" + fullSourceName;
					return output;
				}
				else
					return nullopt; // definition not found
			}
			else if (auto const sourceIdentifier = dynamic_cast<Identifier const*>(sourceNode); sourceIdentifier != nullptr)
			{
				// For identifiers, jump to the naming symbol of the definition of this identifier.
				auto const declaration = !sourceIdentifier->annotation().candidateDeclarations.empty()
					? sourceIdentifier->annotation().candidateDeclarations.front()
					: sourceIdentifier->annotation().referencedDeclaration;

				if (auto const loc = declarationPosition(declaration); loc.has_value())
				{
					Location output{};
					output.range = loc.value();
					output.uri = "file://" + declaration->location().source->name();
					return output;
				}
				else
					return nullopt; // definition not found
			}
			else
			{
				fprintf(stderr, "identifier: %s\n", typeid(*sourceIdentifier).name());
				// LOG: error(_params.requestId, ErrorCode::InvalidParams, "Symbol is not an identifier.");
				return nullopt;
			}
		}
		else
		{
			// error(_params.requestId, ErrorCode::InvalidParams, "Symbol not found.");
			return nullopt;
		}
	}
	else
	{
		// error(_params.requestId, ErrorCode::InvalidRequest, "File not found in VFS.");
		return nullopt;
	}
}

optional<::lsp::Range> LanguageServer::declarationPosition(frontend::Declaration const* _declaration)
{
	if (!_declaration)
		return nullopt;

	auto const location = _declaration->nameLocation();

	auto const [startLine, startColumn] = location.source->translatePositionToLineColumn(location.start);
	auto const [endLine, endColumn] = location.source->translatePositionToLineColumn(location.end);

	return ::lsp::Range{
		{startLine, startColumn},
		{endLine, endColumn}
	};
}

std::vector<LanguageServer::DocumentHighlight> LanguageServer::findAllReferences(frontend::Declaration const* _declaration, SourceUnit const& _sourceUnit)
{
	if (!_declaration)
		return {};

	// XXX the SourceUnit should be the root scope unless we're looking for simple variable identifier.
	// TODO if vardecl, just use decl's scope (for lower overhead).
	return ReferenceCollector::collect(*_declaration, _sourceUnit);
}

void LanguageServer::findAllReferences(
	frontend::Declaration const* _declaration,
	frontend::SourceUnit const& _sourceUnit,
	std::string const& _sourceUnitUri,
	std::vector<Location>& _output
)
{
	for (auto const& highlight: findAllReferences(_declaration, _sourceUnit))
	{
		auto location = Location{};
		location.range = highlight.range;
		location.uri = _sourceUnitUri;
		_output.emplace_back(location);
	}
}


vector<LanguageServer::Location> LanguageServer::references(DocumentPosition _documentPosition)
{
	fprintf(stderr, "find all references: %s:%d:%d\n",
		_documentPosition.uri.c_str(),
		_documentPosition.position.line,
		_documentPosition.position.column
	);

	if (auto const file = m_vfs.find(_documentPosition.uri); file != nullptr)
	{
		if (!m_compilerStack)
			compile(*file);

		solAssert(m_compilerStack.get() != nullptr, "");

		auto const sourceName = file->uri().substr(7); // strip "file://"

		if (auto const sourceNode = findASTNode(_documentPosition.position, sourceName); sourceNode)
		{
			auto output = vector<Location>{};
			if (auto const sourceIdentifier = dynamic_cast<Identifier const*>(sourceNode); sourceIdentifier != nullptr)
			{
				auto const sourceName = _documentPosition.uri.substr(7); // strip "file://"
				frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);

				if (auto decl = sourceIdentifier->annotation().referencedDeclaration; decl)
					findAllReferences(decl, sourceUnit, _documentPosition.uri, output);

				for (auto const decl: sourceIdentifier->annotation().candidateDeclarations)
					findAllReferences(decl, sourceUnit, _documentPosition.uri, output);
			}
			else if (auto const varDecl = dynamic_cast<VariableDeclaration const*>(sourceNode); varDecl != nullptr)
			{
				fprintf(stderr, "AST node is vardecl\n");
				auto const sourceName = _documentPosition.uri.substr(7); // strip "file://"
				frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);
				findAllReferences(varDecl, sourceUnit, _documentPosition.uri, output);
			}
			else
			{
				fprintf(stderr, "not an identifier\n");
			}
			return output;
		}
		else
		{
			fprintf(stderr, "AST node not found\n");
			// error(_params.requestId, ErrorCode::InvalidParams, "Symbol not found.");
			return {};
		}
	}
	else
	{
		// reply(_params.requestId, output);
		// error(_params.requestId, ErrorCode::RequestCancelled, "not implemented yet.");
		return {};
	}
	fflush(stderr);
}

vector<LanguageServer::DocumentHighlight> LanguageServer::semanticHighlight(DocumentPosition _documentPosition)
{
	fprintf(stderr, "DocumentHighlightParams: %s:%d:%d\n",
		_documentPosition.uri.c_str(),
		_documentPosition.position.line,
		_documentPosition.position.column
	);

	if (auto const file = m_vfs.find(_documentPosition.uri); file != nullptr)
	{
		compile(*file);
		solAssert(m_compilerStack.get() != nullptr, "");

		auto const sourceName = file->uri().substr(7); // strip "file://"

		if (auto const sourceNode = findASTNode(_documentPosition.position, sourceName); sourceNode)
		{
			auto output = vector<DocumentHighlight>{};
			if (auto const sourceIdentifier = dynamic_cast<Identifier const*>(sourceNode); sourceIdentifier != nullptr)
			{
				auto const declaration = !sourceIdentifier->annotation().candidateDeclarations.empty()
					? sourceIdentifier->annotation().candidateDeclarations.front()
					: sourceIdentifier->annotation().referencedDeclaration;

				auto const sourceName = _documentPosition.uri.substr(7); // strip "file://"
				frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);
				output = findAllReferences(declaration, sourceUnit);
			}
			else if (auto const varDecl = dynamic_cast<VariableDeclaration const*>(sourceNode); varDecl != nullptr)
			{
				fprintf(stderr, "AST node is vardecl\n");
				auto const sourceName = _documentPosition.uri.substr(7); // strip "file://"
				frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);
				output = findAllReferences(varDecl, sourceUnit);
			}
			else
			{
				fprintf(stderr, "not an identifier\n");
			}
			return output;
		}
		else
		{
			fprintf(stderr, "AST node not found\n");
			// error(_documentPosition.requestId, ErrorCode::InvalidParams, "Symbol not found.");
			return {};
		}
	}
	else
	{
		// reply(_params.requestId, output);
		// error(_documentPosition.requestId, ErrorCode::RequestCancelled, "not implemented yet.");
		return {};
	}
}

} // namespace solidity