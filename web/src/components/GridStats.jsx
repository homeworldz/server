import { createSignal, For, onMount, Show } from "solid-js";
import { getGridStats } from "../lib/api";

// formatCount groups thousands so a five-figure number reads at a glance.
function formatCount(value) {
  return value.toLocaleString();
}

// formatUptime says how long the grid has been up in the two largest units
// that apply, which is how long a person reads it as: "6d 4h", not "534240s".
function formatUptime(seconds) {
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  if (days > 0) {
    return `${days}d ${hours}h`;
  }
  if (hours > 0) {
    return `${hours}h ${minutes}m`;
  }
  return `${minutes}m`;
}

// groups turns one reading into the rows shown, in the order they are shown.
//
// Nothing here substitutes a value. A figure the grid did not publish - an
// uptime on a grid that has not restarted since its event log was added - is
// left out of the list rather than shown as a zero, which would claim the grid
// had just restarted.
function groups(stats, detailed) {
  const people = [
    ["In-world now", formatCount(stats.usersOnline)],
    ["Avatars", formatCount(stats.users)],
    ["Active (30 days)", formatCount(stats.activeUsers30d)],
    ["Active (60 days)", formatCount(stats.activeUsers60d)],
    ["New (30 days)", formatCount(stats.registrations30d)],
  ];
  const land = [
    ["Regions up", `${formatCount(stats.regionsOnline)} of ${formatCount(stats.regions)}`],
    ["Land (256 m regions)", formatCount(stats.regionEquivalents)],
  ];
  const activity = [
    ["Logins (24 hours)", formatCount(stats.logins24h)],
    ["Teleports (24 hours)", formatCount(stats.teleports24h)],
  ];
  if (detailed) {
    land.push(["Regions down", formatCount(stats.regionsOffline)]);
    land.push(["Not deployed", formatCount(stats.regionsUndeployed)]);
    activity.splice(1, 0, ["Logins (30 days)", formatCount(stats.logins30d)]);
    activity.push(["Border crossings (24 hours)", formatCount(stats.crossings24h)]);
  }
  if (typeof stats.uptimeSeconds === "number") {
    activity.push(["Grid uptime", formatUptime(stats.uptimeSeconds)]);
  }
  return [
    { title: "People", rows: people },
    { title: "Land", rows: land },
    { title: "Activity", rows: activity },
  ];
}

// GridStats is the public statistics column, shown beside the login form and
// on the statistics page. It renders nothing at all until the figures arrive:
// a placeholder number on a login page is indistinguishable from a real one,
// and an empty gap for half a second is not.
//
// props.detailed adds the figures the statistics page has room for and the
// login column does not; props.title names the block.
export function GridStats(props) {
  const [stats, setStats] = createSignal(null);
  const [failed, setFailed] = createSignal(false);

  onMount(async () => {
    try {
      setStats(await getGridStats());
    } catch {
      setFailed(true);
    }
  });

  return (
    <Show when={stats() || failed()}>
      <aside class="grid-stats" aria-labelledby="grid-stats-title">
        <h2 id="grid-stats-title">{props.title ?? "Grid statistics"}</h2>
        <Show
          when={stats()}
          fallback={
            <p class="grid-stats-unavailable">
              Statistics are unavailable right now.
            </p>
          }
        >
          <For each={groups(stats(), props.detailed)}>
            {(group) => (
              <div class="grid-stats-group">
                <h3>{group.title}</h3>
                <dl>
                  <For each={group.rows}>
                    {([label, value]) => (
                      <div class="grid-stats-row">
                        <dt>{label}</dt>
                        <dd>{value}</dd>
                      </div>
                    )}
                  </For>
                </dl>
              </div>
            )}
          </For>
          <p class="grid-stats-taken">
            As of{" "}
            <time datetime={stats().capturedAt}>
              {new Date(stats().capturedAt).toLocaleString()}
            </time>
          </p>
        </Show>
      </aside>
    </Show>
  );
}
