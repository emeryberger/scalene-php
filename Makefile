.PHONY: scalene_php test clean

scalene_php: php jemalloc
	cmake -D CMAKE_BUILD_TYPE=Release -B cmake-build-release
	cmake --build cmake-build-release --target all -- -j4
	ln -fs cmake-build-release/libscalene_php.so

test: scalene_php
	./php example.php
	./php scalene.php --cpu-only example.php
	./php scalene.php example.php

php:
ifeq ("$(wildcard php-src)", "")
	git clone 'https://github.com/whuangum/php-src.git'
else
	cd php-src && git pull
endif
	cd php-src && ./buildconf --force
	cd php-src && ./configure --enable-debug --enable-pcntl --enable-maintainer-zts --enable-parallel --with-ffi
	$(MAKE) -C php-src clean
	$(MAKE) -C php-src -j4
	ln -fs php-src/sapi/cli/php

jemalloc:
ifeq ("$(wildcard jemalloc)", "")
	wget 'https://github.com/jemalloc/jemalloc/releases/download/5.2.1/jemalloc-5.2.1.tar.bz2'
	tar -xvjf jemalloc-5.2.1.tar.bz2
	rm jemalloc-5.2.1.tar.bz2
	mv jemalloc-5.2.1 jemalloc
endif
	cd jemalloc && ./configure --with-jemalloc-prefix=je_
	$(MAKE) -C jemalloc clean
	$(MAKE) -C jemalloc -j4

clean:
	cmake --build cmake-build-release --target clean -- -j 4
	rm -rf cmake-build-release
