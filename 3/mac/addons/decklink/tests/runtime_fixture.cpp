#ifdef VTRPON_DECKLINK_NO_FACTORY
extern "C" void vtrpon_runtime_fixture() {}
#else
extern "C" void *CreateDeckLinkAPIInformationInstance_0001() { return nullptr; }
#endif
