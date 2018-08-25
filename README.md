# process
守护目标进程，当进程退出代码不在设置范围内则自动重启该进程

# `process.config` 配置文件格式
```JSON
{
	"log": "log.log",
	"target": "./runable.exe",
	"code": [1]
}
```
