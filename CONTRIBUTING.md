# 贡献指南

感谢你对本项目的关注与支持 ❤️

## 🚀 参与开发

### 相关版本

- Qt 6.6.3
- MSVC 2019 64bit
- C++17、CMake 3.16+、Ninja；运行时需要 Git

### 核心测试

备份核心独立于界面和第三方组件，可以单独构建：

```sh
cmake -S . -B build/core -G Ninja -DZCVERSIONBOX_BUILD_APP=OFF -DBUILD_TESTING=ON -DCMAKE_PREFIX_PATH="/path/to/Qt/6.6.3/kit"
cmake --build build/core --parallel
ctest --test-dir build/core --output-on-failure
```

构建桌面应用时将 `ZCVERSIONBOX_BUILD_APP` 设为 `ON`。人工验收请用 `--data-dir <临时目录>` 和临时源文件，避免操作真实备份。自动测试包括事务中断、原生事件监听、远端快进及分叉、备注和万文件增量备份；详见 [备份引擎文档](docs/backup-engine.md)。

### 项目结构

```
3rdparty                # 第三方库
└─xxx
    ├─include           # 头文件
    ├─bin               # 可执行文件或动态库（运行时依赖）
    └─lib               # 静态/动态链接库
backup                  # 备份服务、调度、原生监控、Git 存储与恢复事务
application             # 单实例请求转发、独立 AI 备注队列
tests                   # Qt Test 核心与进程测试
res                     # 资源文件
└─img                   # 图片资源
utils                   # 工具类/辅助函数代码
windows                 # 主窗口模块
└─mainwindow_child
    ├─homepage          # 主页窗口
    └─settingpage       # 设置窗口
```

