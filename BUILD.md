## Building and Installing on Host

The Facebook dependencies are:
* The Folly library from https://github.com/facebook/folly
* The Fizz library from https://github.com/facebookincubator/fizz
* The thrift library from https://github.com/apache/thrift
* The rsocket-cpp library from https://github.com/rsocket/rsocket-cpp
* The proxygen library from https://github.com/facebook/proxygen
* The Wangle library from https://github.com/facebook/wangle
* The FBThrift library from https://github.com/facebook/fbthrift

Other librarys (Ubuntu): 


```
sudo apt install libzstd-dev libfmt-dev flex bison libssl-dev zlib1g-dev libboost-all-dev libgoogle-glog-dev libgflags-dev libsqlite3-dev libsodium-dev
sudo apt-get install \
    g++ \
    cmake \
    libboost-all-dev \
    libevent-dev \
    libdouble-conversion-dev \
    libgoogle-glog-dev \
    libgflags-dev \
    libiberty-dev \
    liblz4-dev \
    liblzma-dev \
    libsnappy-dev \
    make \
    zlib1g-dev \
    binutils-dev \
    libjemalloc-dev \
    libssl-dev \
    pkg-config
```



Install FB Deps:

```
git clone https://github.com/facebook/folly && cd folly && mkdir _build && cd _build && cmake .. -DBUILD_SHARED_LIBS=on
make -j10 && sudo make install
```

Then install other libraries

```
git clone https://github.com/facebookincubator/fizz && cd fizz/fizz && mkdir _build && cd _build && cmake .. -DBUILD_SHARED_LIBS=on
make -j10 && make install

git clone https://github.com/apache/thrift && cd thrift && mkdir _build && cd _build && cmake .. -DBUILD_SHARED_LIBS=on
make -j10 && make install

git clone https://github.com/rsocket/rsocket-cpp && cd rsocket-cpp && mkdir _build && cd _build && cmake .. -DBUILD_SHARED_LIBS=on
make -j10 && make install

git clone https://github.com/facebook/proxygen && cd proxygen && mkdir _build && cd _build && cmake .. -DBUILD_SHARED_LIBS=on
make -j10 && make install

git clone https://github.com/facebook/wangle && cd wangle && mkdir _build && cd _build && cmake .. -DBUILD_SHARED_LIBS=on
make -j10 && make install

git clone https://github.com/facebook/fbthrift && cd fbthrift && mkdir _build && cd _build && cmake .. -DBUILD_SHARED_LIBS=on
make -j10 && make install

```


Build Bistro

```
git clone https://github.com/facebook/bistro 
cd bistro
./bistro/cmake/run-cmake.sh Debug
cd ./bistro/cmake/Debug
make -j10

#build sucess!
```

Goto https://github.com/facebook/bistro#your-first-bistro-run Run First Demo.
