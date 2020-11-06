library(dplyr)
library(jsonlite)
library(ggplot2)
library(tidyr)

setwd("~/Downloads")

# load data
data_linux <- fromJSON("linux.json")

# process data
data_linux %>%
  unlist() %>%
  tibble::enframe() %>%
  separate(name, c("script", "mode", "id"), sep = '\\.') %>%
  separate(script, c("program", "script_id"), sep = '-') %>%
  replace_na(list(script_id = 1)) %>%
  select(-c(id)) %>%
  rename(times = value) %>%
  mutate(program = factor(program)) %>%
  mutate(script_id = factor(script_id)) %>%
  mutate(mode = factor(mode)) ->
  run_times_linux

# relative run times
run_times_linux %>%
  group_by(program, script_id) %>%
  mutate(rel_times = times / min(times[mode == "base"])) %>%
  ungroup() %>%
  group_by(program, script_id, mode) %>%
  summarize(
    min = min(rel_times),
    first_quantile = quantile(rel_times, 0.25),
    median = median(rel_times),
    mean = mean(rel_times),
    third_quantile = quantile(rel_times, 0.75),
    max = max(rel_times),
    .groups = "keep") %>%
  knitr::kable()

run_times_linux %>%
  group_by(program, script_id) %>%
  mutate(rel_times = times / min(times[mode == "base"])) %>%
  ungroup() %>%
  ggplot(aes(x = mode, y = rel_times)) +
  geom_boxplot() +
  ylab("time (sec) / (min time for this script in base mode)") +
  labs(title = "Linux") +
  coord_flip()

run_times_linux %>%
  group_by(program, script_id) %>%
  mutate(times = times / min(times[mode == "base"])) %>%
  ungroup() %>%
  ggplot(aes(x = script_id, y = times, fill = mode)) +
  geom_col(position = "dodge2") +
  facet_wrap(vars(program)) +
  ylab("time (sec) / (min time for this script in base mode)") +
  labs(title = "Linux") +
  coord_flip()

# actual run times
run_times_linux %>%
  ggplot(aes(x = script_id, y = times, fill = mode)) +
  geom_col(position = "dodge2") +
  facet_wrap(vars(program)) +
  ylab("time (sec)") +
  labs(title = "Linux") +
  coord_flip()
