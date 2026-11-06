/* Calls the real openDAQ libraries through the generated C header (output/bindings/c/opendaq_abi.h).
   Built and run by checks/test_c_header.py; the libraries are loaded at run time from OPENDAQ_BIN. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "opendaq_abi.h"

static HMODULE coretypes, opendaq;
static int failures = 0;

#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL %s\n", what); failures++; } else printf("ok   %s\n", what); } while (0)
#define LOAD(lib, name) ((PFN_##name)GetProcAddress(lib, #name))
#define OK(err) (((err) & 0x80000000u) == 0)

typedef daqErrCode (*PFN_createString)(daqIString**, const char*);
typedef daqErrCode (*PFN_createList)(daqIList**);
typedef daqErrCode (*PFN_createInteger)(daqIInteger**, int64_t);
typedef daqErrCode (*PFN_createInstanceBuilder)(daqIInstanceBuilder**);
typedef daqErrCode (*PFN_createInstanceFromBuilder)(daqIInstance**, daqIInstanceBuilder*);

static const char* str(daqIBaseObject* obj)
{
    daqIString* s = NULL;
    const char* text = NULL;
    if (obj && OK(obj->vtbl->queryInterface(obj, &daqIID_IString, (void**) &s)))
    {
        s->vtbl->getCharPtr(s, &text);
        s->vtbl->releaseRef(s);
    }
    return text;
}

int main(void)
{
    const char* bin = getenv("OPENDAQ_BIN");
    char path[1024];
    if (!bin) { printf("set OPENDAQ_BIN\n"); return 2; }
    SetDllDirectoryA(bin);
    snprintf(path, sizeof path, "%s\\daqcoretypes-64-3.dll", bin);
    coretypes = LoadLibraryA(path);
    snprintf(path, sizeof path, "%s\\opendaq-64-3.dll", bin);
    opendaq = LoadLibraryA(path);
    if (!coretypes || !opendaq) { printf("cannot load the libraries from %s\n", bin); return 2; }

    /* strings, interface ids, queryInterface */
    daqIString* s = NULL;
    CHECK(OK(LOAD(coretypes, createString)(&s, "hello")), "createString");
    const char* text = NULL;
    s->vtbl->getCharPtr(s, &text);
    CHECK(text && strcmp(text, "hello") == 0, "daqIString::getCharPtr through the generated vtable");
    size_t len = 0;
    s->vtbl->getLength(s, &len);
    CHECK(len == 5, "daqIString::getLength");
    daqIConvertible* conv = NULL;
    CHECK(OK(s->vtbl->queryInterface(s, &daqIID_IConvertible, (void**) &conv)), "queryInterface daqIConvertible with the generated id");
    daqIList* notList = NULL;
    CHECK(s->vtbl->queryInterface(s, &daqIID_IList, (void**) &notList) == OPENDAQ_ERR_NOINTERFACE, "queryInterface daqIList fails with OPENDAQ_ERR_NOINTERFACE");
    daqCoreType ct = 0;
    daqICoreType* coreType = NULL;
    s->vtbl->queryInterface(s, &daqIID_ICoreType, (void**) &coreType);
    coreType->vtbl->getCoreType(coreType, &ct);
    CHECK(ct == daqCoreType_ctString, "daqICoreType::getCoreType returns the generated enum value");

    /* lists */
    daqIList* list = NULL;
    LOAD(coretypes, createList)(&list);
    daqIInteger* i42 = NULL;
    LOAD(coretypes, createInteger)(&i42, 42);
    list->vtbl->pushBack(list, (daqIBaseObject*) i42);
    list->vtbl->pushBack(list, (daqIBaseObject*) s);
    size_t count = 0;
    list->vtbl->getCount(list, &count);
    CHECK(count == 2, "daqIList::pushBack and getCount");
    daqIBaseObject* item = NULL;
    list->vtbl->getItemAt(list, 1, &item);
    CHECK(str(item) && strcmp(str(item), "hello") == 0, "daqIList::getItemAt");
    CHECK(list->vtbl->getItemAt(list, 7, &item) == OPENDAQ_ERR_OUTOFRANGE, "out-of-range error code from the generated defines");

    /* an instance with the reference modules */
    daqIInstanceBuilder* builder = NULL;
    LOAD(opendaq, createInstanceBuilder)(&builder);
    daqIString* modulePath = NULL;
    LOAD(coretypes, createString)(&modulePath, bin);
    builder->vtbl->setModulePath(builder, modulePath);
    daqIInstance* instance = NULL;
    CHECK(OK(LOAD(opendaq, createInstanceFromBuilder)(&instance, builder)), "createInstanceFromBuilder");
    daqIString* cs = NULL;
    LOAD(coretypes, createString)(&cs, "daqref://device0");
    daqIDevice* device = NULL;
    CHECK(OK(instance->vtbl->addDevice(instance, &device, cs, NULL)), "daqIDevice::addDevice (slot inherited through daqIFolder, daqIComponent, daqIPropertyObject)");
    daqIString* name = NULL;
    device->vtbl->getName(device, &name);
    CHECK(str((daqIBaseObject*) name) != NULL, "daqIComponent::getName on the added device");
    printf("     device name: %s\n", str((daqIBaseObject*) name));
    daqIList* channels = NULL;
    device->vtbl->getChannels(device, &channels, NULL);
    channels->vtbl->getCount(channels, &count);
    CHECK(count > 0, "daqIDevice::getChannels");

    printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
