# MiniOB 基础训练营 18 题精简版

从 OceanBase MiniOB 派生，保留基础训练营 18 题所需的 SQL 与行存、B+ 树索引、事务和持久化，以及内置终端和评测连接。项目沿用 Mulan PSL v2 许可证，见 License 和 NOTICE。

## 在 Ubuntu 24.04 或 WSL 中编译

在项目目录运行以下命令。系统需要 C++20 编译器、Make、CMake、Flex、Bison 和 Python 3。仓库通过子模块固定引用 libevent、JsonCpp 和 replxx 的版本。

```bash
bash build.sh init
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_ASAN=OFF
cmake --build build --target observer -j4
```

## 打开数据库

```bash
./build/bin/observer -f ./etc/observer.ini -P cli
```

输入 `SELECT 1;`，可以看到查询结果。输入 `exit` 退出。数据库默认使用启动目录下的 `miniob` 目录保存数据。

## 本地验证

```bash
python3 test/basic_training_regression.py
```

脚本按 18 个题目分组测试，在临时目录启动数据库。
