VERSION=18.05.2

all:
	$(MAKE) -C src

mac:
	$(MAKE) -C src mac

android:
	$(MAKE) -C src android

install:
	$(MAKE) -C src install

clean:
	$(MAKE) -C src clean

spkg:
	git clean -fdx
	gbp buildpackage --git-debian-tag="v%s" --git-upstream-branch=master --git-debian-branch=ubuntu --git-upstream-tag='v%(version)s' --git-ignore-new -tc -pgpg2 -S
	git checkout debian
	sed -i s/trusty/precise/g debian/changelog
	gbp buildpackage --git-debian-tag="v%s" --git-upstream-branch=master --git-debian-branch=ubuntu --git-upstream-tag='v%(version)s'  --git-ignore-new -tc -pgpg2 -S
	git checkout debian
	sed -i s/trusty/xenial/g debian/changelog
	gbp buildpackage --git-debian-tag="v%s" --git-upstream-branch=master --git-debian-branch=ubuntu --git-upstream-tag='v%(version)s'  --git-ignore-new -tc -pgpg2 -S
	git checkout debian
	sed -i s/trusty/bionic/g debian/changelog
	gbp buildpackage --git-debian-tag="v%s" --git-upstream-branch=master --git-debian-branch=ubuntu --git-upstream-tag='v%(version)s'  --git-ignore-new -tc -pgpg2 -S
	git checkout debian
	git clean -fdx

upload:
	git push origin v$(VERSION)
	dput ppa:jamey-hicks/connectal ../fpgajtag_$(VERSION)-*_source.changes
