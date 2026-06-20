#ifndef _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_
#define _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_

#include "smsdk_ext.h"
#include <convar.h>

class Webcon: public SDKExtension, public IConCommandBaseAccessor
{
public:
	virtual bool SDK_OnLoad(char *error, size_t maxlength, bool late);
	virtual void SDK_OnUnload();
	virtual bool QueryInterfaceDrop(SMInterface *interface);
	virtual void NotifyInterfaceDrop(SMInterface *interface);
	virtual bool SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlength, bool late);

public: // IConCommandBaseAccessor
	bool RegisterConCommandBase(ConCommandBase *pCommandBase);
};

#endif // _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_
