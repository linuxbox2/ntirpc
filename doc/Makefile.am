EXTRA_DIST = etc_netconfig

install: install-am
	$(mkinstalldirs) $(DESTDIR)/$(sysconfdir)
	cp -p $(srcdir)/etc_netconfig $(DESTDIR)/$(sysconfdir)/netconfig
	chmod 0644 $(DESTDIR)/$(sysconfdir)/netconfig

uninstall: uninstall-am
	$(RM) $(DESTDIR)/$(sysconfdir)/netconfig
