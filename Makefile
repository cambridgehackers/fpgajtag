
all:
	make -C src

mac:
	make -C src mac

install:
	make -C src install

clean:
	make -C src clean

VERSION=14.10.1

dpkg:
	git buildpackage --git-debian-tag="v%s" --git-upstream-branch=master --git-debian-branch=ubuntu/trusty --git-ignore-new -tc

