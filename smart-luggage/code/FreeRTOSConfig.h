#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* FreeRTOSConfig.h —— FreeRTOS 内核配置（目标：STM32F103C8 / 72 MHz / 20 KB SRAM）。

   每个数值都按本工程的实际情况定过，改动前先看清对应注释里的理由。
   带【改】标记的是相对最初模板动过的地方 —— 也是答辩时可能被追问的点。

   本工程实际用到的内核功能：抢占式调度、时间片、互斥量、任务通知、事件组。
   未使用（对应宏均为 0）：软件定时器、协程、队列集、计数信号量、递归互斥量、
   静态创建、运行时间统计。

   参数含义的官方说明见 http://www.freertos.org/a00110.html */

#define configUSE_PREEMPTION		1   /* 1 = 抢占式：高优先级任务一就绪就立刻抢 */

/* ============================ 钩子函数 ============================ */

/* 空闲钩子。每个空闲任务周期被调用一次，常用来让 CPU 进省电模式。
   本工程不用（= 0）。注意钩子里不能调用会引起空闲任务阻塞的 API。 */
#define configUSE_IDLE_HOOK			0  //空闲时 钩子函数 回调函数

/* 时间片钩子。每个 tick 调用一次，必须极短、且不能调以 FromISR 结尾的 API。
   本工程不用（= 0）。 */
#define configUSE_TICK_HOOK			0

/* 内存申请失败钩子。= 1 时 pvPortMalloc 失败会调用 vApplicationMallocFailedHook()。
   【本工程靠它保命】—— xTaskCreate / xQueueCreate 建失败不会崩、也不会报错，
   只会让那个任务或队列静默消失；有了这个钩子才会打到串口并停机（实现在 main.c）。
   20 KB RAM 上跑 6 个任务，堆不够用是真实风险之一，所以必须开。 */
#define configUSE_MALLOC_FAILED_HOOK			1  

/* ============================ 时钟与优先级 ============================ */

#define configCPU_CLOCK_HZ			( ( unsigned long ) 72000000 )
#define configTICK_RATE_HZ			( ( TickType_t ) 1000 )   /* 1 tick = 1 ms */
#define configMAX_PRIORITIES		( 32 ) //可使用的最大优先级
#define configMINIMAL_STACK_SIZE	( ( unsigned short ) 128 )   //是128字，并非字节 即128*4个字节，堆栈大小以字为单位

/* FreeRTOS 堆大小。6 个任务的栈、TCB、以及所有队列/信号量都从这里分配，
   所以它必须容得下：6 个任务栈合计 1888 字 = 7552 B，外加 6 个 TCB 和几个队列。

   STM32F103C8 只有 20 KB SRAM，原模板值 17*1024 是给 F103ZE 等 64 KB RAM 芯片的，
   在本芯片上会导致 RAM 占用 97%、只剩几百字节余量 —— 再加任何全局变量就报
   L6406E "No space in execution regions"。20 KB 的 F103C8 按资料与实测建议取 4~10 KB。

   【改】当前取 12 KB：堆余量约 2.9 KB，RAM 总占用约 18.4 KB / 20 KB 仍安全
   （主栈 MSP 1 KB + C 库堆 512 B 都还放得下）。 */
#define configTOTAL_HEAP_SIZE		( ( size_t ) ( 12 * 1024 ) )
#define configMAX_TASK_NAME_LEN		( 16 )   /* 任务名长度上限，栈溢出钩子靠它认出是谁爆了 */
#define configUSE_16_BIT_TICKS		0 //系统节拍计数器变量数据类型，1表示为16位无符号整形，0表示为32位无符号整形
#define configIDLE_SHOULD_YIELD		1 //1:空闲任务放弃CPU使用权,给其他同优先级的用户任务  2:空闲优先级和其他优先级相同。避免2，多使用1

#define configUSE_TIME_SLICING      1 //时间片调度，当优先级相同时执行


#define configUSE_QUEUE_SETS        0 //队列 为1开启，为0关闭



#define configUSE_TASK_NOTIFICATIONS 1 //开启任务通知功能，默认开启；本工程用它做"请求 comm 上报锁状态"


#define configUSE_MUTEXES           1  //互斥信号量开关（用于保护 OLED 共享外设）


                                           
#define configUSE_RECURSIVE_MUTEXES			0   //使用递归互斥信号量 


#define configUSE_COUNTING_SEMAPHORES		0  //为1时使用计数信号量

/* 设置可以注册的信号量和消息队列个数 */
#define configQUEUE_REGISTRY_SIZE			10                                 
                                                                       
#define configUSE_APPLICATION_TASK_TAG		 0         

/* ============================ 内存申请 ============================ */
//支持动态内存申请
#define configSUPPORT_DYNAMIC_ALLOCATION     1    
//支持静态内存
#define configSUPPORT_STATIC_ALLOCATION		 0					

/* 大于 0 时启用栈溢出检测。1 和 2 是两种检测方法：
   1 = 只在任务切换时检查栈指针是否越界（快）；2 = 还检查栈末尾的填充字节是否被改写
   （更早发现，稍慢）。本工程用 2，配合 vApplicationStackOverflowHook 打出任务名。 */
#define configCHECK_FOR_STACK_OVERFLOW		 2   

/* ============================ 运行时间统计 / 协程 ============================ */
//启用运行时间统计功能
#define configGENERATE_RUN_TIME_STATS	     0             
 //启用可视化跟踪调试（同时为1时会编译 vTaskList / vTaskGetRunTimeStats）
#define configUSE_TRACE_FACILITY			 0    

//启用协程，启用协程以后必须添加文件croutine.c
#define configUSE_CO_ROUTINES 			        0                 
//协程的有效优先级数目
#define configMAX_CO_ROUTINE_PRIORITIES       ( 2 )                   

/* ============================ 软件定时器（本工程未使用） ============================ */
 //启用软件定时器
#define configUSE_TIMERS				        0                              
//软件定时器优先级
#define configTIMER_TASK_PRIORITY		      (configMAX_PRIORITIES-1)        
//软件定时器队列长度
#define configTIMER_QUEUE_LENGTH		        10                               
//软件定时器任务堆栈大小
#define configTIMER_TASK_STACK_DEPTH	      (configMINIMAL_STACK_SIZE*2)    

/* ============================ 调度器实现方式 ============================
   选择"下一个要运行的任务"有两种办法：
     · 通用方法：纯 C 实现，效率略低，但可用任意优先级数目
     · 硬件方法（= 1）：用 Cortex-M3 的前导零指令（CLZ）加速查找，
                        效率更高，代价是最大优先级被限定为 32

   【改】模板里这一行的宏名中间多打了一个空格（configUSE_PORT_OPTIMISED_TASK SELECTION），
   等于根本没定义这个宏。已修正为正确拼写。 */
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  1

/* tickless 低功耗模式。= 1 时系统空闲会停掉 tick 中断以省电；
   = 0 保持节拍中断一直运行。
   【本工程用 0】—— 开了之后下载会变得麻烦（程序在睡眠中，需要"按住复位再点下载"
   或先擦除芯片）。本设备虽然电池供电，但续航优化不是当前目标，保持简单。 */
#define configUSE_TICKLESS_IDLE   0

/* 把 FreeRTOS 的汇编入口挂到标准库的异常名上（Cortex-M3 的惯例） */
#define xPortPendSVHandler              PendSV_Handler 
#define vPortSVCHandler                 SVC_Handler

/* ============================ 可选函数 ============================ */
#define INCLUDE_xTaskGetSchedulerState       1                       
#define INCLUDE_vTaskPrioritySet		     1
#define INCLUDE_uxTaskPriorityGet		     1
#define INCLUDE_vTaskDelete				     1
#define INCLUDE_vTaskCleanUpResources	     1
#define INCLUDE_vTaskSuspend			     1
#define INCLUDE_vTaskDelayUntil			     1
#define INCLUDE_vTaskDelay				     1
#define INCLUDE_eTaskGetState			     1
#define INCLUDE_xTimerPendFunctionCall	     0
//#define INCLUDE_xTaskGetCurrentTaskHandle       1
//#define INCLUDE_uxTaskGetStackHighWaterMark     0
//#define INCLUDE_xTaskGetIdleTaskHandle          0

/* ============================ 中断优先级（最容易配错的一块） ============================ */
#ifdef __NVIC_PRIO_BITS
	#define configPRIO_BITS       		__NVIC_PRIO_BITS
#else
	#define configPRIO_BITS       		4                  
#endif
//中断最低优先级
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY			15     

/* 受 FreeRTOS 管控的最高中断优先级（数值）。

   ⚠️ 这一条的语义最容易记反，记牢：
       数值【>= 5】的中断受 FreeRTOS 管控 —— 允许在其中调用 xxxFromISR() 系列 API；
       数值【0~4】的中断不受管控 —— 绝不能在其中调用【任何】FreeRTOS API。
   （数值越小优先级越高，所以"更紧急的中断"反而不能用 RTOS 的东西。）

   本工程三个 USART 中断和 EXTI1 都设成 6，符合要求。设错的话 configASSERT 会在
   第一次收数据的瞬间直接断言停机。 */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY	5

#define configKERNEL_INTERRUPT_PRIORITY 		( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) )	/* 240 */

#define configMAX_SYSCALL_INTERRUPT_PRIORITY 	( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) )

/* 与 ST 库的约定一致：ST 库允许 16 级优先级（0~15），内核优先级取 15（最低）。 */
#define configLIBRARY_KERNEL_INTERRUPT_PRIORITY	15

#endif /* FREERTOS_CONFIG_H */
