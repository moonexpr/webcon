#include "extension.h"

#include <utility>
#include <cstdint>
#include <cctype>

#include "microhttpd.h"

#include <fcntl.h>

#ifdef _WIN32
#include <io.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#define closesocket close
#define ioctlsocket ioctl
#define WSAGetLastError() errno
#endif

#include "sm_namehashset.h"

#include "IConplex.h"
#include "extension_spew.h"

ICvar *icvar = nullptr;
ConVar webcon_debug("webcon_debug", "0", FCVAR_NONE,
	"Enable verbose webcon debug logging (stderr/file via WebLogger).");

// Runtime-gated verbose spew. Toggle live with `webcon_debug 1`. Routed through
// WebLogger so it lands with the forwarded libmicrohttpd error spew.
#define WDLOG(...) do { if (webcon_debug.GetBool()) WebLogger_Log(__VA_ARGS__); } while (0)

Webcon g_Webcon;
SMEXT_LINK(&g_Webcon);

IConplex *conplex;

MHD_Daemon *httpDaemon;
MHD_Response *responseNotFound;

static const size_t WEB_MAX_METHOD_LENGTH = 32;
static const size_t WEB_MAX_URL_LENGTH = 2048;
static const size_t WEB_MAX_HANDLER_ID_LENGTH = 64;

static int QueuePlainResponse(MHD_Connection *connection, unsigned int status, const char *body)
{
	MHD_Response *response = MHD_create_response_from_buffer(strlen(body), (void *)body, MHD_RESPMEM_PERSISTENT);
	MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, "text/plain; charset=UTF-8");
	int ok = MHD_queue_response(connection, status, response);
	MHD_destroy_response(response);
	return ok;
}

static bool WebMethodIsValid(const char *method)
{
	if (method == NULL || method[0] == '\0') {
		return false;
	}

	size_t length = strlen(method);
	if (length > WEB_MAX_METHOD_LENGTH) {
		return false;
	}

	for (size_t i = 0; i < length; i++) {
		unsigned char c = (unsigned char)method[i];
		if (!isupper(c) && !isdigit(c) && c != '-') {
			return false;
		}
	}

	return true;
}

static bool WebUrlIsValid(const char *url)
{
	if (url == NULL || url[0] != '/') {
		return false;
	}

	size_t length = strlen(url);
	if (length > WEB_MAX_URL_LENGTH) {
		return false;
	}

	for (size_t i = 0; i < length; i++) {
		char c = url[i];
		if (c == '\r' || c == '\n') {
			return false;
		}
	}

	if (strstr(url, "..") != NULL) {
		return false;
	}

	return true;
}

struct PluginRequestHandler
{
	static bool matches(const char *key, const PluginRequestHandler &value);
	static uint32_t hash(const SourceMod::detail::CharsAndLength &key);

	PluginRequestHandler(const char *id, IPluginContext *context, funcid_t function, const char *name, const char *description);
	~PluginRequestHandler();

	PluginRequestHandler(PluginRequestHandler &&other);

	PluginRequestHandler(PluginRequestHandler const &other) = delete;
	PluginRequestHandler &operator =(PluginRequestHandler const &other) = delete;

	bool IsAlive();
	bool Execute(MHD_Connection *connection, const char *method, const char *url);

	IChangeableForward *callback;
	char *id;
	char *name;
	char *description;
};

bool PluginRequestHandler::matches(const char *key, const PluginRequestHandler &value)
{
	return (strcmp(key, value.id) == 0);
}

uint32_t PluginRequestHandler::hash(const SourceMod::detail::CharsAndLength &key)
{
	return key.hash();
}

PluginRequestHandler::PluginRequestHandler(const char *id, IPluginContext *context, funcid_t function, const char *name, const char *description)
{
	callback = forwards->CreateForwardEx(NULL, ET_Single, 3, NULL, Param_Cell, Param_String, Param_String);
	callback->AddFunction(context, function);

	this->id = strdup(id);
	this->name = strdup(name);
	this->description = strdup(description);
}

PluginRequestHandler::PluginRequestHandler(PluginRequestHandler &&other)
{
	callback = other.callback;
	id = other.id;
	name = other.name;
	description = other.description;

	other.callback = NULL;
	other.id = NULL;
	other.name = NULL;
	other.description = NULL;
}

PluginRequestHandler::~PluginRequestHandler()
{
	if (callback) {
		forwards->ReleaseForward(callback);
	}

	free(id);
	free(name);
	free(description);
}

bool PluginRequestHandler::IsAlive()
{
	return (callback->GetFunctionCount() > 0);
}

bool PluginRequestHandler::Execute(MHD_Connection *connection, const char *method, const char *url)
{
	if (!WebMethodIsValid(method) || !WebUrlIsValid(url)) {
		return false;
	}

	Handle_t handle = (Handle_t)(uintptr_t)(MHD_get_connection_info(connection, MHD_CONNECTION_INFO_SOCKET_CONTEXT)->socket_context);

	if (handle == BAD_HANDLE) {
		return false;
	}

	callback->PushCell(handle);
	callback->PushString(method);
	callback->PushString(url);

	cell_t result = 0;
	callback->Execute(&result);

	return (result != 0);
}

static int InvokeRequestHandler(MHD_Connection *connection, PluginRequestHandler &handler, const char *method, const char *url)
{
	if (!WebMethodIsValid(method)) {
		return QueuePlainResponse(connection, MHD_HTTP_BAD_REQUEST, "Bad Request");
	}

	if (!WebUrlIsValid(url)) {
		return QueuePlainResponse(connection, MHD_HTTP_BAD_REQUEST, "Bad Request");
	}

	if (!handler.Execute(connection, method, url)) {
		return QueuePlainResponse(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Internal Server Error");
	}

	return MHD_YES;
}

const char *defaultRequestHandler;
NameHashSet<PluginRequestHandler> requestHandlers;

HandleType_t handleTypeResponse;

struct ResponseTypeHandler: public IHandleTypeDispatch
{
	void OnHandleDestroy(HandleType_t type, void *object);
};

ResponseTypeHandler handlerResponseType;

void ResponseTypeHandler::OnHandleDestroy(HandleType_t type, void *object)
{
	MHD_destroy_response((MHD_Response *)object);
}

HandleType_t handleTypeConnection;

struct ConnectionTypeHandler: public IHandleTypeDispatch
{
	void OnHandleDestroy(HandleType_t type, void *object);
};

ConnectionTypeHandler handlerConnectionType;

void ConnectionTypeHandler::OnHandleDestroy(HandleType_t type, void *object)
{
	// Do nothing.
}

cell_t WebResponse_AddHeader(IPluginContext *context, const cell_t *params)
{
	HandleSecurity security;
	security.pOwner = context->GetIdentity();
	security.pIdentity = myself->GetIdentity();

	MHD_Response *response = NULL;
	HandleError error = handlesys->ReadHandle(params[1], handleTypeResponse, &security, (void **)&response);
	if (error != HandleError_None) {
		return context->ThrowNativeError("Invalid response handle %x (error %d)", params[1], error);
	}

	char *header = NULL;
	context->LocalToString(params[2], &header);

	char *content = NULL;
	context->LocalToString(params[3], &content);

	return MHD_add_response_header(response, header, content);
}

cell_t WebResponse_RemoveHeader(IPluginContext *context, const cell_t *params)
{
	HandleSecurity security;
	security.pOwner = context->GetIdentity();
	security.pIdentity = myself->GetIdentity();

	MHD_Response *response = NULL;
	HandleError error = handlesys->ReadHandle(params[1], handleTypeResponse, &security, (void **)&response);
	if (error != HandleError_None) {
		return context->ThrowNativeError("Invalid response handle %x (error %d)", params[1], error);
	}

	char *header = NULL;
	context->LocalToString(params[2], &header);

	char *content = NULL;
	context->LocalToStringNULL(params[3], &content);
	
	if (content) {
		return MHD_del_response_header(response, header, content);
	}
	
	bool success = false;
	const char *value;
	while ((value = MHD_get_response_header(response, header))) {
		success = MHD_del_response_header(response, header, value) || success;
	}
	
	return success ? MHD_YES : MHD_NO;
}

cell_t WebStringResponse_WebStringResponse(IPluginContext *context, const cell_t *params)
{
	char *content = NULL;
	context->LocalToString(params[1], &content);

	MHD_Response *response = MHD_create_response_from_buffer(strlen(content), (void *)content, MHD_RESPMEM_MUST_COPY);

	return handlesys->CreateHandle(handleTypeResponse, response, context->GetIdentity(), myself->GetIdentity(), NULL);
}

cell_t WebBinaryResponse_WebBinaryResponse(IPluginContext *context, const cell_t *params)
{
	char *content = NULL;
	context->LocalToString(params[1], &content);

	MHD_Response *response = MHD_create_response_from_buffer(params[2], (void *)content, MHD_RESPMEM_MUST_COPY);

	return handlesys->CreateHandle(handleTypeResponse, response, context->GetIdentity(), myself->GetIdentity(), NULL);
}

cell_t WebFileResponse_WebFileResponse(IPluginContext *context, const cell_t *params)
{
	char *path = NULL;
	context->LocalToString(params[1], &path);

	char realPath[PLATFORM_MAX_PATH] = {};
	smutils->BuildPath(Path_Game, realPath, sizeof(realPath), "%s", path);

#ifdef _WIN32
	int fd = _open(realPath, _O_RDONLY | _O_BINARY | _O_SEQUENTIAL);
#else
	int fd = open(realPath, O_RDONLY);
#endif

	if (fd == -1) {
		return context->ThrowNativeError("Failed to open \"%s\" (error %d)", path, errno);
	}

#ifdef _WIN32
	long size = _lseek(fd, 0, SEEK_END);
#else
	off_t size = lseek(fd, 0, SEEK_END);
#endif

	MHD_Response *response = MHD_create_response_from_fd(size, fd);

	return handlesys->CreateHandle(handleTypeResponse, response, context->GetIdentity(), myself->GetIdentity(), NULL);
}

cell_t WebConnection_GetClientAddress(IPluginContext *context, const cell_t *params)
{
	HandleSecurity security;
	security.pOwner = context->GetIdentity();
	security.pIdentity = myself->GetIdentity();

	MHD_Connection *connection;
	HandleError error = handlesys->ReadHandle(params[1], handleTypeConnection, &security, (void **)&connection);
	if (error != HandleError_None) {
		return context->ThrowNativeError("Invalid connection handle %x (error %d)", params[1], error);
	}

	sockaddr_in *address = (sockaddr_in *)MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS)->client_addr;
	char *ip = inet_ntoa(address->sin_addr);
	context->StringToLocal(params[2], params[3], ip);

	return 1;
}

cell_t WebConnection_GetRequestData(IPluginContext *context, const cell_t *params)
{
	HandleSecurity security;
	security.pOwner = context->GetIdentity();
	security.pIdentity = myself->GetIdentity();

	MHD_Connection *connection;
	HandleError error = handlesys->ReadHandle(params[1], handleTypeConnection, &security, (void **)&connection);
	if (error != HandleError_None) {
		return context->ThrowNativeError("Invalid connection handle %x (error %d)", params[1], error);
	}

	MHD_ValueKind kind;
	switch (params[2]) {
		case 0:
			kind = MHD_GET_ARGUMENT_KIND;
			break;
		case 1:
			kind = MHD_POSTDATA_KIND;
			break;
		case 2:
			kind = MHD_COOKIE_KIND;
			break;
		case 3:
			kind = MHD_HEADER_KIND;
			break;
		default:
			return context->ThrowNativeError("Unknown WebRequestDataType %d", params[2]);
	}

	char *key;
	context->LocalToString(params[3], &key);

	const char *value = MHD_lookup_connection_value(connection, kind, key);
	if (!value) {
		return 0;
	}
	
	context->StringToLocal(params[4], params[5], value);

	return 1;
}

cell_t WebConnection_QueueResponse(IPluginContext *context, const cell_t *params)
{
	HandleError error;

	HandleSecurity security;
	security.pOwner = context->GetIdentity();
	security.pIdentity = myself->GetIdentity();

	MHD_Connection *connection;
	error = handlesys->ReadHandle(params[1], handleTypeConnection, &security, (void **)&connection);
	if (error != HandleError_None) {
		return context->ThrowNativeError("Invalid connection handle %x (error %d)", params[1], error);
	}

	MHD_Response *response;
	error = handlesys->ReadHandle(params[3], handleTypeResponse, &security, (void **)&response);
	if (error != HandleError_None) {
		return context->ThrowNativeError("Invalid response handle %x (error %d)", params[3], error);
	}

	return MHD_queue_response(connection, params[2], response);;
}

cell_t Web_RegisterRequestHandler(IPluginContext *context, const cell_t *params)
{
	char *id = NULL;
	context->LocalToString(params[1], &id);
	if (strlen(id) == 0) {
		return 0;
	}

	NameHashSet<PluginRequestHandler>::Insert i = requestHandlers.findForAdd(id);

	if (i.found()) {
		if (i->IsAlive()) {
			return 0;
		}

		if (defaultRequestHandler && PluginRequestHandler::matches(defaultRequestHandler, *i)) {
			defaultRequestHandler = NULL;
		}

		requestHandlers.remove(i);

		i = requestHandlers.findForAdd(id);
	}

	char *name = NULL;
	context->LocalToString(params[3], &name);

	char *description = NULL;
	context->LocalToString(params[4], &description);

	PluginRequestHandler handler(id, context, params[2], name, description);

	// TODO: Test code
	//defaultRequestHandler = handler.id;

	requestHandlers.add(i, std::move(handler));

	return 1;
}

sp_nativeinfo_t natives[] = {
	{"WebResponse.AddHeader", WebResponse_AddHeader},
	{"WebResponse.RemoveHeader", WebResponse_RemoveHeader},
	{"WebStringResponse.WebStringResponse", WebStringResponse_WebStringResponse},
	{"WebBinaryResponse.WebBinaryResponse", WebBinaryResponse_WebBinaryResponse},
	{"WebFileResponse.WebFileResponse", WebFileResponse_WebFileResponse},
	{"WebConnection.GetClientAddress", WebConnection_GetClientAddress},
	{"WebConnection.GetRequestData", WebConnection_GetRequestData},
	{"WebConnection.QueueResponse", WebConnection_QueueResponse},
	{"Web_RegisterRequestHandler", Web_RegisterRequestHandler},
	{NULL, NULL}
};

int DefaultConnectionHandler(void *cls, MHD_Connection *connection, const char *url, const char *method, const char *version, const char *upload_data, size_t *upload_data_size, void **con_cls)
{
	WDLOG("DefaultConnectionHandler: method=%s url=%s version=%s", method, url, version);

	if (url[0] != '/') {
		WebLogger_Log("DefaultConnectionHandler: URL does not start with '/': \"%s\"", url);
		return MHD_NO;
	}

	if (defaultRequestHandler) {
		// TODO: Check for remapped handlers.

		NameHashSet<PluginRequestHandler>::Result i = requestHandlers.find(defaultRequestHandler);
		assert(i.found()); // It should have always been cleaned up before getting here.

		if (i->IsAlive()) {
			int result = InvokeRequestHandler(connection, *i, method, url);
			WDLOG("defaultRequestHandler dispatch returned %d", result);
			return result;
		} else {
			defaultRequestHandler = NULL;

			requestHandlers.remove(i);
		}
	}

	if (url[1] == '\0') {
		size_t length = 51;
		char *buffer = (char *)malloc(length + 1);
		if (!buffer) {
			return MHD_NO;
		}

		size_t cursor = 0;
		cursor += sprintf(buffer, "<!DOCTYPE html>\n<html><body><dl>");

		for (NameHashSet<PluginRequestHandler>::iterator i = requestHandlers.iter(); !i.empty(); i.next()) {
			if (!i->IsAlive()) {
				if (defaultRequestHandler && PluginRequestHandler::matches(defaultRequestHandler, *i)) {
					defaultRequestHandler = NULL;
				}

				i.erase();

				continue;
			}

			const char *name = i->name;
			bool noName = (name[0] == '\0');
			if (noName) {
				name = i->id;
			}

			const char *description = i->description;
			if (description[0] == '\0') {
				description = "<i>No Description</i>";
			}

			length += 35 + strlen(i->id) + strlen(name) + strlen(description);
			if (noName) {
				length += 7;
			}

			buffer = (char *)realloc(buffer, length + 1);
			if (!buffer) {
				return MHD_NO;
			}

			// TODO: Escape these.
			cursor += sprintf(buffer + cursor, "<dt><a href=\"/%s/\">%s%s%s</a></dt><dd>%s</dd>", i->id, (noName ? "<i>" : ""), name, (noName ? "</i>" : ""), description);
		}

		cursor += sprintf(buffer + cursor, "</dl></body></html>");

		assert(cursor == length);

		MHD_Response *response = MHD_create_response_from_buffer(length, (void *)buffer, MHD_RESPMEM_MUST_FREE);
		int success = MHD_queue_response(connection, MHD_HTTP_OK, response);
		MHD_destroy_response(response);

		return success;
	}

	const char *id = url + 1;
	const char *path = "/";
	const char *end = strchr(id, '/');
	char *buffer = NULL;

	if (end) {
		size_t length = (end - id);
		if (length > WEB_MAX_HANDLER_ID_LENGTH) {
			return QueuePlainResponse(connection, MHD_HTTP_REQUEST_URI_TOO_LONG, "URI Too Long");
		}

		buffer = (char *)malloc(length + 1);

		if (!buffer) {
			return MHD_NO;
		}

		strncpy(buffer, id, length);
		buffer[length] = '\0';

		path = end;
		id = buffer;
	} else if (strlen(id) > WEB_MAX_HANDLER_ID_LENGTH) {
		return QueuePlainResponse(connection, MHD_HTTP_REQUEST_URI_TOO_LONG, "URI Too Long");
	}

	NameHashSet<PluginRequestHandler>::Result i = requestHandlers.find(id);

	free(buffer);

	bool found = i.found();

	if (found && !i->IsAlive()) {
		if (defaultRequestHandler && PluginRequestHandler::matches(defaultRequestHandler, *i)) {
			defaultRequestHandler = NULL;
		}

		requestHandlers.remove(i);

		found = false;
	}

	if (!found) {
		return MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, responseNotFound);
	}

	// While it's cleaner to do this above, redirecting to a 404 seems... bad.
	if (!end) {
		size_t length = strlen(url);
		char *redirect = (char *)malloc(length + 2);
		if (!redirect) {
			return MHD_NO;
		}

		strcpy(redirect, url);
		redirect[length] = '/';
		redirect[length + 1] = '\0';

		MHD_Response *response = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
		MHD_add_response_header(response, MHD_HTTP_HEADER_LOCATION, redirect);
		int success = MHD_queue_response(connection, MHD_HTTP_FOUND, response);
		MHD_destroy_response(response);

		free(redirect);

		return success;
	}

	int result = InvokeRequestHandler(connection, *i, method, path);
	WDLOG("handler '%s' dispatch returned %d", id, result);
	return result;
}

void LogErrorCallback(void *cls, const char *fm, va_list ap)
{
	char buffer[2048];
	size_t bytes = smutils->FormatArgs(buffer, sizeof(buffer), fm, ap);
	buffer[bytes - 1] = '\0'; // Strip newline.
	smutils->LogError(myself, "%s", buffer);

	// Also forward to WebLogger for stderr/file spew (visible on the server console).
	WebLogger_Log("%s", buffer);
}

void NotifyConnectionCallback(void *cls, MHD_Connection *connection, void **socket_context, MHD_ConnectionNotificationCode toe)
{
	Handle_t *handle = (Handle_t *)socket_context;

	HandleError error;

	HandleSecurity security;
	security.pOwner = NULL;
	security.pIdentity = myself->GetIdentity();

	switch(toe) {
		case MHD_CONNECTION_NOTIFY_STARTED:
		{
			*handle = handlesys->CreateHandle(handleTypeConnection, connection, NULL, myself->GetIdentity(), &error);

			if (*handle == BAD_HANDLE) {
				smutils->LogError(myself, "Error creating handle for connection. (%d)", error);
			}

			break;
		}

		case MHD_CONNECTION_NOTIFY_CLOSED:
		{
			if (*handle == BAD_HANDLE) {
				break;
			}

			error = g_pHandleSys->FreeHandle(*handle, &security);

			// We can't control SM freeing our handles on unload.
			// This should be safe as MHD_stop_daemon is called inside SDK_OnUnload.
			if (error != HandleError_None && error != HandleError_Freed) {
				smutils->LogError(myself, "Error freeing handle for connection. (%x, %d)", *handle, error);
			}

			*handle = BAD_HANDLE;

			break;
		}
	}
}

IConplex::ProtocolDetectionState ConplexHTTPDetector(const char *id, const unsigned char *buffer, unsigned int bufferLength)
{
	if (bufferLength == 0) {
		return IConplex::NeedMoreData;
	}

	if (buffer[0] < 'A' || buffer[0] > 'Z') {
		return IConplex::NoMatch;
	}

	bool hasSpace = false;
	bool hasSlash = false;
	for (unsigned int i = 0; i < bufferLength; ++i) {
		if (hasSpace) {
			hasSlash = (buffer[i] == '/');
			return hasSlash ? IConplex::Match : IConplex::NoMatch;
		}
		
		hasSpace = (i >= 3) && (buffer[i] == ' ');
		if (hasSpace) {
			continue;
		}
		
		if (buffer[i] < 'A' || buffer[i] > 'Z') {
			return IConplex::NoMatch;
		}
	}
	
	return IConplex::NeedMoreData;
}

bool ConplexHTTPHandler(const char *id, int socket, const sockaddr *address, unsigned int addressLength)
{
	WDLOG("ConplexHTTPHandler: socket=%d", socket);
	int ret = MHD_add_connection(httpDaemon, socket, address, addressLength);
	if (ret == MHD_NO) {
		WebLogger_Log("MHD_add_connection failed for socket %d", socket);
	}
	return true; // MHD will close the socket on failure.
}

IConplex::ProtocolDetectionState ConplexHTTPSDetector(const char *id, const unsigned char *buffer, unsigned int bufferLength)
{
	if (bufferLength <= 0) return IConplex::NeedMoreData;
	if (buffer[0] != 0x16) return IConplex::NoMatch;
	if (bufferLength <= 1) return IConplex::NeedMoreData;
	if (buffer[1] != 0x03) return IConplex::NoMatch;
	if (bufferLength <= 5) return IConplex::NeedMoreData;
	if (buffer[5] != 0x01) return IConplex::NoMatch;
	if (bufferLength <= 6) return IConplex::NeedMoreData;
	if (buffer[6] != 0x00) return IConplex::NoMatch;
	if (bufferLength <= 8) return IConplex::NeedMoreData;
	if (((buffer[3] * 256) + buffer[4]) != ((buffer[7] * 256) + buffer[8] + 4)) return IConplex::NoMatch;
	return IConplex::Match;
}

bool ConplexHTTPSHandler(const char *id, int socket, const sockaddr *address, unsigned int addressLength)
{
	// We don't actually handle HTTPS connections yet.
	// Implementation will be the same as ConplexHTTPHandler apart from handing off to a dedicated HTTPS daemon.
	return false;
}

void OnGameFrame(bool simulating)
{
	MHD_run(httpDaemon);
}

bool Webcon::SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	GET_V_IFACE_CURRENT(GetEngineFactory, icvar, ICvar, CVAR_INTERFACE_VERSION);
	g_pCVar = icvar;
	ConVar_Register(0, this);
	return true;
}

bool Webcon::RegisterConCommandBase(ConCommandBase *pCommandBase)
{
	return META_REGCVAR(pCommandBase);
}

bool Webcon::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
	sharesys->AddDependency(myself, "conplex.ext", true, true);

	SM_GET_IFACE(CONPLEX, conplex);
	
	if (!conplex->RegisterProtocolHandler("HTTP", ConplexHTTPDetector, ConplexHTTPHandler)) {
		strncpy(error, "Failed to register handler for HTTP protocol", maxlength);
		return false;
	}
	
	if (!conplex->RegisterProtocolHandler("HTTPS", ConplexHTTPSDetector, ConplexHTTPSHandler)) {
		conplex->DropProtocolHandler("HTTP");
		strncpy(error, "Failed to register handler for HTTPS protocol", maxlength);
		return false;
	}

	httpDaemon = MHD_start_daemon(MHD_USE_DEBUG | MHD_USE_NO_LISTEN_SOCKET, 0, NULL, NULL, &DefaultConnectionHandler, NULL, MHD_OPTION_EXTERNAL_LOGGER, LogErrorCallback, NULL, MHD_OPTION_NOTIFY_CONNECTION, NotifyConnectionCallback, NULL, MHD_OPTION_END);
	if (!httpDaemon) {
		conplex->DropProtocolHandler("HTTP");
		conplex->DropProtocolHandler("HTTPS");
		strncpy(error, "Failed to start HTTP server", maxlength);
		return false;
	}

	const char *contentNotFound = "Not Found";
	responseNotFound = MHD_create_response_from_buffer(strlen(contentNotFound), (void *)contentNotFound, MHD_RESPMEM_PERSISTENT);
	MHD_add_response_header(responseNotFound, MHD_HTTP_HEADER_CONTENT_TYPE, "text/plain; charset=UTF-8");
	
	handleTypeResponse = handlesys->CreateType("WebResponse", &handlerResponseType, 0, NULL, NULL, myself->GetIdentity(), NULL);

	HandleAccess connectionAccessRules;
	g_pHandleSys->InitAccessDefaults(NULL, &connectionAccessRules);

	connectionAccessRules.access[HandleAccess_Delete] = HANDLE_RESTRICT_IDENTITY;

	handleTypeConnection = handlesys->CreateType("WebConnection", &handlerConnectionType, 0, NULL, &connectionAccessRules, myself->GetIdentity(), NULL);

	sharesys->AddNatives(myself, natives);
	
	smutils->AddGameFrameHook(OnGameFrame);

	return true;
}

void Webcon::SDK_OnUnload()
{
	smutils->RemoveGameFrameHook(OnGameFrame);
	
	if (conplex) {
		conplex->DropProtocolHandler("HTTP");
		conplex->DropProtocolHandler("HTTPS");
	}
	
	MHD_destroy_response(responseNotFound);

	MHD_stop_daemon(httpDaemon);

	WebLogger_Shutdown();

	handlesys->RemoveType(handleTypeResponse, myself->GetIdentity());
	handlesys->RemoveType(handleTypeConnection, myself->GetIdentity());
}

bool Webcon::QueryInterfaceDrop(SMInterface *interface)
{
	if (conplex && interface == conplex) {
		return false;
	}

	return true;
}

void Webcon::NotifyInterfaceDrop(SMInterface *interface)
{
	if (conplex && interface == conplex) {
		conplex->DropProtocolHandler("HTTP");
		conplex->DropProtocolHandler("HTTPS");
		conplex = NULL;
	}
}
