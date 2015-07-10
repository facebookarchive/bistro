You should consider using Bistro instead of rolling your own solution,
because in our experience at Facebook, every heavily used task-execution
service eventually has to build out a very similar set of layers:

* **Execution shell:** Start, wait for, kill tasks
* **Worker pool:** Maintain the distributed state of what tasks are running where
* **Data model:** Currently available tasks; resource accounting
* **Configuration:** Add/remove jobs; tune parameters
* **Persistence:** If the service crashes, don't forget task completions
* **Scheduling logic:** Priorities; dependencies; resource constraints;
host placement
* **Task logs:** Make logs accessible, while controlling disk usage
* **Command-line tools:** Scriptable, yet friendly
* **Web UI:** Minimal learning curve; great for visualizing data
* **RPC API:** When command-line scripting is too slow or clunky

Bistro provides all of the above, and improves continuously, so instead of
re-inventing, you can focus on innovating.
