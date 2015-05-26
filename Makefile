
all:
	make -C src

mac:
	make -C src mac

install:
	make -C src install

clean:
	make -C src clean

VERSION=14.10.1

spkg:
	git clean -fdx
	git buildpackage --git-debian-tag="v%s" --git-upstream-branch=master --git-debian-branch=ubuntu --git-upstream-tag='v%(version)s' --git-ignore-new -tc -S
	sed -i s/trusty/precise/g debian/changelog
	git buildpackage --git-debian-tag="v%s" --git-upstream-branch=master --git-debian-branch=ubuntu --git-upstream-tag='v%(version)s'  --git-ignore-new -tc -S
	sed -i s/precise/utopic/g debian/changelog
	git buildpackage --git-debian-tag="v%s" --git-upstream-branch=master --git-debian-branch=ubuntu --git-upstream-tag='v%(version)s'  --git-ignore-new -tc -S
	git checkout debian
	sed -i s/trusty/vivid/g debian/changelog
	git buildpackage --git-debian-tag="v%s" --git-upstream-branch=master --git-debian-branch=ubuntu --git-upstream-tag='v%(version)s'  --git-ignore-new -tc -S
	git checkout debian
	git clean -fdx

upload:
	git push origin v$(VERSION)
	dput ppa:jamey-hicks/connectal ../fpgajtag_$(VERSION)-*_source.changes
	(cd  ../obs/home:jameyhicks:connectaldeb/fpgajtag/; osc rm * || true)
	cp -v ../fpgajtag_$(VERSION)*trusty*.diff.gz ../fpgajtag_$(VERSION)*trusty*.dsc ../fpgajtag_$(VERSION)*.orig.tar.gz ../obs/home:jameyhicks:connectaldeb/fpgajtag/
	(cd ../obs/home:jameyhicks:connectaldeb/fpgajtag/; osc add *; osc commit -m $(VERSION) )
	(cd ../obs/home:jameyhicks:connectal/fpgajtag; sed -i "s/>v.....</>v$(VERSION)</" _service; osc commit -m "v$(VERSION)" )
