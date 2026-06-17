// End-to-end functional test for the x64 conplex + webcon extensions.
// Registers a webcon request handler under the virtual host "e2etest". A request
// to TCP 27015 (the game port) must be: accepted by the game server, detected as
// HTTP and routed by conplex (CSocketCreator::ProcessAccept detour), handed to
// webcon, dispatched to this handler, and answered. Exercises all three x64
// extensions end to end. Test with:  curl -H "Host: e2etest" http://<ip>:27015/
#pragma semicolon 1
#pragma newdecls required

#include <sourcemod>
#include <webcon>

WebResponse gResponse;

public void OnPluginStart()
{
	if (!Web_RegisterRequestHandler("e2etest", OnWebRequest, "E2E Test", "x64 end-to-end test")) {
		SetFailState("Failed to register webcon request handler.");
	}
	gResponse = new WebStringResponse("E2E-OK conplex+webcon x64\n");
	PrintToServer("[WEB-E2E] handler registered (virtual host: e2etest)");
}

public bool OnWebRequest(WebConnection connection, const char[] method, const char[] url)
{
	char addr[WEB_CLIENT_ADDRESS_LENGTH];
	connection.GetClientAddress(addr, sizeof(addr));
	PrintToServer("[WEB-E2E] request from %s: %s %s", addr, method, url);
	return connection.QueueResponse(WebStatus_OK, gResponse);
}
