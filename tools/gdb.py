import gdb

def exit_if_not_crashed(event):
    if hasattr(event, "exit_code"):
        gdb.execute("quit")

gdb.events.exited.connect(exit_if_not_crashed)
