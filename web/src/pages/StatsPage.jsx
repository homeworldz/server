import { GridStats } from "../components/GridStats";
import { GRID_BASE_URL } from "../config";

// The statistics page: the same figures the login page shows in its column,
// with the ones there is no room for beside a login form, and a note saying
// what "active" means so the numbers cannot be read as something else.
export function StatsPage() {
  return (
    <section class="stats-page" aria-labelledby="stats-title">
      <header class="stats-header">
        <h1 id="stats-title">Grid statistics</h1>
        <p class="lede">
          Counted from the grid's own records, not estimated. An active user is
          a distinct person who logged in during the window, however many times
          they logged in.
        </p>
      </header>

      <GridStats title="Now" detailed />

      <p class="auth-alt">
        The same figures are recorded once a day as a row of{" "}
        <a href={`${GRID_BASE_URL}/stats.csv`}>stats.csv</a>, which is
        the whole history in one plain-text file.
      </p>
    </section>
  );
}
