
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
	sed -i s/precise/trusty/g debian/changelog
	git buildpackage --git-debian-tag="v%s" --git-upstream-branch=master --git-debian-branch=ubuntu/trusty --git-ignore-new -tc -S
	sed -i s/trusty/precise/g debian/changelog
	git buildpackage --git-debian-tag="v%s" --git-upstream-branch=master --git-debian-branch=ubuntu/trusty --git-ignore-new -tc -S

